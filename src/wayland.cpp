#include "vw/wayland.hpp"
#include "vw/animation.hpp"
#include "vw/app.hpp"
#include "vw/host.hpp"
#include "vw/ipc.hpp"
#include "roles/role.hpp"

#include <wlr-data-control-unstable-v1-zwayland-client.h>
#include <zwwm-focus-grab-v1-zwayland-client.h>
#include <cursor-shape-zwayland-client.h>
#include <linux-dmabuf-zwayland-client.h>
#include <wayland-zwayland-client.h>
#include <zwayland/client/core.hpp>
#include <zwayland/client/display.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <limits>
#include <chrono>
#include <cmath>
#include <poll.h>
#include <optional>
#include <utility>
#include <vector>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/inotify.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <sys/mman.h>

namespace vw {
namespace {

using namespace zwayland;

// Bumped by every input-driven rebuild. Surfaces share application state, so
// the driver rebuilds each surface whose last rebuild is older than this.
std::uint64_t global_input_epoch = 0;
constexpr std::chrono::milliseconds drag_frame_limit{33};

// What this process last copied. While one of its sources owns the
// selection, pastes are answered from here: asking the compositor would make
// this thread both the reader and the writer of the transfer.
struct Clipboard {
  std::string text;
  std::uint64_t generation = 0;
  bool owned = false;
  const WaylandWidget* owner = nullptr;
};
Clipboard clipboard;
// The surface that most recently received input; its connection carries
// vw::set_clipboard and vw::clipboard_text.
WaylandWidget* clipboard_surface = nullptr;

constexpr const char* text_types[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING",
                                      "STRING", "TEXT"};

// Writes all of text to fd, giving a stalled reader at most a second.
void write_all(int fd, std::string_view text) {
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!text.empty() && std::chrono::steady_clock::now() < deadline) {
    const ssize_t written = write(fd, text.data(), text.size());
    if (written > 0) {
      text.remove_prefix(static_cast<std::size_t>(written));
    } else if (written < 0 && (errno == EAGAIN || errno == EINTR)) {
      pollfd ready{fd, POLLOUT, 0};
      (void)poll(&ready, 1, 100);
    } else {
      break;
    }
  }
}

struct Globals {
  roles::Role* role = nullptr;
  std::uint32_t compositor = 0;
  std::uint32_t compositor_version = 0;
  std::uint32_t dmabuf = 0;
  std::uint32_t dmabuf_version = 0;
  std::uint32_t focus_grab_manager = 0;
  std::uint32_t focus_grab_version = 0;
  std::uint32_t seat = 0;
  std::uint32_t seat_version = 0;
  std::uint32_t data_device_manager = 0;
  std::uint32_t data_device_manager_version = 0;
  std::uint32_t data_control_manager = 0;
  std::uint32_t data_control_manager_version = 0;
  std::uint32_t cursor_shape_manager = 0;
  std::uint32_t cursor_shape_manager_version = 0;

  void global(client::Proxy&, std::uint32_t name, const std::string& interface,
              std::uint32_t version) {
    if (interface == generated::wl_compositor_interface.name) {
      compositor = name;
      compositor_version = version;
    } else if (interface == generated::zwp_linux_dmabuf_v1_interface.name) {
      dmabuf = name;
      dmabuf_version = version;
    } else if (interface == generated::zwwm_focus_grab_manager_v1_interface.name) {
      focus_grab_manager = name;
      focus_grab_version = version;
    } else if (interface == generated::wl_seat_interface.name) {
      seat = name;
      seat_version = version;
    } else if (interface == generated::wl_data_device_manager_interface.name) {
      data_device_manager = name;
      data_device_manager_version = version;
    } else if (interface == generated::zwlr_data_control_manager_v1_interface.name) {
      data_control_manager = name;
      data_control_manager_version = version;
    } else if (interface == generated::wp_cursor_shape_manager_v1_interface.name) {
      cursor_shape_manager = name;
      cursor_shape_manager_version = version;
    } else {
      role->global(name, interface, version);
    }
  }

  void global_remove(client::Proxy&, std::uint32_t) {}
};

// Handlers are user code; a throwing handler must not take down the loop.
template <typename Handler, typename... Arguments>
void invoke(const Handler& handler, const char* what, Arguments&&... arguments) {
  if (!handler) return;
  try {
    handler(std::forward<Arguments>(arguments)...);
  } catch (const std::exception& error) {
    std::cerr << "vw: " << what << " callback failed: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "vw: " << what << " callback failed\n";
  }
}

// Whether two scenes look and behave the same to the compositor, so the
// newer one can be adopted (for its fresh input handlers) without rendering.
// Custom shaders read only the scene, so they need no exception.
bool same_presentation(const Scene& a, const Scene& b) {
  return a.width == b.width && a.height == b.height && a.anchor == b.anchor &&
         a.layer == b.layer && a.margin == b.margin && a.exclusive == b.exclusive &&
         a.exclusive_zone == b.exclusive_zone && a.accepts_input == b.accepts_input &&
         a.selective_input == b.selective_input && a.focus_grab == b.focus_grab &&
         a.input_regions == b.input_regions && a.surface_namespace == b.surface_namespace &&
         a.title == b.title && a.app_id == b.app_id && a.decorations == b.decorations &&
         a.rectangles == b.rectangles &&
         a.rasters == b.rasters;
}

bool interaction_contains(const Interaction& interaction, double x, double y) {
  const auto& clip = interaction.clip;
  if (clip[2] > 0.0F && clip[3] > 0.0F &&
      (x < clip[0] || x >= clip[0] + clip[2] || y < clip[1] || y >= clip[1] + clip[3]))
    return false;
  return x >= interaction.x && x < interaction.x + interaction.width &&
         y >= interaction.y && y < interaction.y + interaction.height;
}

}  // namespace

struct WaylandWidget::KeyboardState {
  xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  xkb_keymap* keymap = nullptr;
  xkb_state* state = nullptr;

  ~KeyboardState() {
    if (state != nullptr) xkb_state_unref(state);
    if (keymap != nullptr) xkb_keymap_unref(keymap);
    if (context != nullptr) xkb_context_unref(context);
  }

  void set_keymap(const char* text) {
    if (state != nullptr) xkb_state_unref(state);
    if (keymap != nullptr) xkb_keymap_unref(keymap);
    keymap = xkb_keymap_new_from_string(context, text, XKB_KEYMAP_FORMAT_TEXT_V1,
                                        XKB_KEYMAP_COMPILE_NO_FLAGS);
    state = keymap == nullptr ? nullptr : xkb_state_new(keymap);
  }
};

WaylandWidget::WaylandWidget(const Scene& scene, const Frame& frame)
    : display_(zwayland::client::Display::connect()), scene_(scene),
      width_(frame.width), height_(frame.height) {
  if (!display_) throw std::runtime_error("could not connect to the Wayland compositor");
  role_ = roles::make_role(scene_, *this);
  discover_globals();
  if (roles::uses_widget_buffer(scene_)) create_buffer(frame);
  role_->create();
  // Data control works without input, so the first surface can serve the
  // clipboard until another one receives input.
  if (clipboard_surface == nullptr) clipboard_surface = this;
}

WaylandWidget::~WaylandWidget() {
  if (display_ && focus_grab_ != nullptr) {
    try {
      generated::zwwm_focus_grab_v1_destroy(*display_, focus_grab_->id);
      generated::zwwm_focus_grab_manager_v1_destroy(*display_, focus_grab_manager_->id);
      (void)display_->flush();
    } catch (...) {}
  }
  set_focus_grab_state(false, false);
  if (display_ && role_) role_->destroy();
  set_session_locked(false);
  if (clipboard_surface == this) clipboard_surface = nullptr;
  // The selection dies with this connection.
  if (clipboard.owner == this) {
    clipboard.owned = false;
    clipboard.owner = nullptr;
  }
}

void WaylandWidget::discover_globals() {
  using namespace zwayland;
  client::Proxy* registry = client::core::get_registry(*display_);
  if (registry == nullptr) throw std::runtime_error("could not create the Wayland registry");
  registry_ = registry;
  Globals globals{role_.get()};
  struct RegistryEvents {
    Globals* globals;
    void global(client::Proxy& proxy, std::uint32_t name, const std::string& interface,
                std::uint32_t version) {
      globals->global(proxy, name, interface, version);
    }
    void global_remove(client::Proxy& proxy, std::uint32_t name) {
      globals->global_remove(proxy, name);
    }
  };
  registry->set_observer(generated::wl_registry_observer(RegistryEvents{&globals}));
  if (!display_->roundtrip()) throw std::runtime_error("Wayland registry roundtrip failed");
  if (globals.compositor == 0 || globals.dmabuf == 0)
    throw std::runtime_error("compositor requires wl_compositor and linux-dmabuf");
  compositor_ = client::core::bind(
      *display_, *registry, globals.compositor, generated::wl_compositor_interface,
      std::min(globals.compositor_version, generated::wl_compositor_interface.version));
  dmabuf_ = client::core::bind(*display_, *registry, globals.dmabuf,
      generated::zwp_linux_dmabuf_v1_interface, std::min(3U, globals.dmabuf_version));
  if (compositor_ == nullptr || dmabuf_ == nullptr)
    throw std::runtime_error("could not bind required Wayland globals");
  // Over vw's surfaces the cursor is vw's to set; cursor-shape names it and
  // the compositor draws it from its theme.
  if (globals.cursor_shape_manager != 0)
    cursor_shape_manager_ = client::core::bind(*display_, *registry, globals.cursor_shape_manager,
        generated::wp_cursor_shape_manager_v1_interface,
        std::min(globals.cursor_shape_manager_version, generated::wp_cursor_shape_manager_v1_interface.version));
  if (globals.seat != 0) {
    seat_ = client::core::bind(*display_, *registry, globals.seat, generated::wl_seat_interface,
                               std::min(globals.seat_version, generated::wl_seat_interface.version));
    if (seat_ == nullptr) throw std::runtime_error("could not bind wl_seat");
    struct SeatEvents {
      WaylandWidget* widget;
      void capabilities(client::Proxy&, std::uint32_t capabilities) {
        if ((capabilities & generated::WL_SEAT_CAPABILITY_POINTER) != 0U) create_pointer();
        if ((capabilities & generated::WL_SEAT_CAPABILITY_KEYBOARD) != 0U)
          widget->create_keyboard();
      }
      void name(client::Proxy&, const std::string&) {}

     private:
      void create_pointer() { widget->create_pointer(); }
    };
    seat_->set_observer(generated::wl_seat_observer(SeatEvents{this}));
    if (globals.data_device_manager != 0) {
      data_manager_ = client::core::bind(*display_, *registry, globals.data_device_manager,
          generated::wl_data_device_manager_interface,
          std::min(3U, globals.data_device_manager_version));
      if (data_manager_ != nullptr) create_data_device();
    }
    if (globals.data_control_manager != 0) {
      control_manager_ = client::core::bind(*display_, *registry, globals.data_control_manager,
          generated::zwlr_data_control_manager_v1_interface,
          std::min(2U, globals.data_control_manager_version));
      if (control_manager_ != nullptr) create_control_device();
    }
    if (!display_->roundtrip()) throw std::runtime_error("Wayland seat roundtrip failed");
  }
  if (role_->focus_grab() && globals.focus_grab_manager != 0) {
    focus_grab_manager_ = client::core::bind(*display_, *registry, globals.focus_grab_manager,
        generated::zwwm_focus_grab_manager_v1_interface,
        std::min(globals.focus_grab_version, generated::zwwm_focus_grab_manager_v1_interface.version));
    set_focus_grab_state(focus_grab_manager_ != nullptr, false);
  }
  // Globals was a discovery-local object; don't retain a dangling registry
  // observer when the compositor later advertises/removes another global.
  // A role that follows the registry replaces this in bind().
  registry->set_observer([](client::Proxy& proxy, std::uint32_t opcode, wire::MessageParser& parser) {
    parser.discard(proxy.interface->events[opcode].signature);
  });
  role_->bind();
}

void WaylandWidget::create_data_device() {
  const auto id = generated::wl_data_device_manager_get_data_device(
      *display_, data_manager_->id, seat_);
  data_device_ = display_->find_proxy(id);
  if (data_device_ == nullptr) throw std::runtime_error("could not create wl_data_device");
  struct OfferEvents {
    WaylandWidget* widget;
    void offer(client::Proxy& proxy, const std::string& type) {
      widget->offer_types_[proxy.id].push_back(type);
    }
    void source_actions(client::Proxy&, std::uint32_t) {}
    void action(client::Proxy&, std::uint32_t) {}
  };
  struct DeviceEvents {
    WaylandWidget* widget;
    void data_offer(client::Proxy&, client::Proxy* offer) {
      widget->offer_types_[offer->id];
      offer->set_observer(generated::wl_data_offer_observer(OfferEvents{widget}));
    }
    // Drag and drop is not supported; offers made for it are released.
    void enter(client::Proxy&, std::uint32_t, client::Proxy*, double, double,
               client::Proxy* offer) {
      widget->forget_offer(widget->drag_offer_);
      widget->drag_offer_ = offer;
    }
    void leave(client::Proxy&) { widget->forget_offer(widget->drag_offer_); }
    void motion(client::Proxy&, std::uint32_t, double, double) {}
    void drop(client::Proxy&) { widget->forget_offer(widget->drag_offer_); }
    void selection(client::Proxy&, client::Proxy* offer) {
      if (widget->selection_offer_ != offer) widget->forget_offer(widget->selection_offer_);
      widget->selection_offer_ = offer;
    }
  };
  data_device_->set_observer(generated::wl_data_device_observer(DeviceEvents{this}));
}

void WaylandWidget::create_control_device() {
  const auto id = generated::zwlr_data_control_manager_v1_get_data_device(
      *display_, control_manager_->id, seat_);
  control_device_ = display_->find_proxy(id);
  if (control_device_ == nullptr) throw std::runtime_error("could not create data control device");
  struct OfferEvents {
    WaylandWidget* widget;
    void offer(client::Proxy& proxy, const std::string& type) {
      widget->offer_types_[proxy.id].push_back(type);
    }
  };
  struct DeviceEvents {
    WaylandWidget* widget;
    void data_offer(client::Proxy&, client::Proxy* offer) {
      widget->offer_types_[offer->id];
      offer->set_observer(generated::zwlr_data_control_offer_v1_observer(OfferEvents{widget}));
    }
    void selection(client::Proxy&, client::Proxy* offer) {
      if (widget->control_offer_ != offer) widget->forget_control_offer(widget->control_offer_);
      widget->control_offer_ = offer;
    }
    // Only the regular clipboard is used; primary selection offers are released.
    void primary_selection(client::Proxy&, client::Proxy* offer) {
      if (widget->control_primary_offer_ != offer)
        widget->forget_control_offer(widget->control_primary_offer_);
      widget->control_primary_offer_ = offer;
    }
    // The compositor withdrew data control; fall back to wl_data_device.
    void finished(client::Proxy&) {
      widget->control_offer_ = nullptr;
      widget->control_primary_offer_ = nullptr;
      widget->control_device_ = nullptr;
    }
  };
  control_device_->set_observer(
      generated::zwlr_data_control_device_v1_observer(DeviceEvents{this}));
}

void WaylandWidget::forget_control_offer(client::Proxy*& offer) {
  if (offer == nullptr) return;
  offer_types_.erase(offer->id);
  generated::zwlr_data_control_offer_v1_destroy(*display_, offer->id);
  offer = nullptr;
}

void WaylandWidget::forget_offer(client::Proxy*& offer) {
  if (offer == nullptr) return;
  offer_types_.erase(offer->id);
  generated::wl_data_offer_destroy(*display_, offer->id);
  offer = nullptr;
}

namespace {

// Serves this process's clipboard text to whoever pastes; the same events
// arrive for wl_data_source and zwlr_data_control_source_v1.
struct SourceEvents {
  std::vector<std::pair<std::uint32_t, bool>>* dead;
  std::uint64_t generation;
  bool control;
  void target(client::Proxy&, const std::optional<std::string>&) {}
  void send(client::Proxy&, const std::string&, int fd) {
    write_all(fd, clipboard.text);
    close(fd);
  }
  // Destroyed after dispatch: not from inside its own event handler.
  void cancelled(client::Proxy& proxy) {
    if (clipboard.generation == generation) {
      clipboard.owned = false;
      clipboard.owner = nullptr;
    }
    dead->emplace_back(proxy.id, control);
  }
  void dnd_drop_performed(client::Proxy&) {}
  void dnd_finished(client::Proxy&) {}
  void action(client::Proxy&, std::uint32_t) {}
};

}  // namespace

void WaylandWidget::copy_to_clipboard(std::string text) {
  const bool control = control_manager_ != nullptr && control_device_ != nullptr;
  // wl_data_device needs keyboard focus and a recent input serial.
  if (!control && (data_manager_ == nullptr || data_device_ == nullptr || input_serial_ == 0)) return;
  clipboard.text = std::move(text);
  const std::uint64_t generation = ++clipboard.generation;
  clipboard.owned = true;
  clipboard.owner = this;
  const auto id = control
      ? generated::zwlr_data_control_manager_v1_create_data_source(*display_, control_manager_->id)
      : generated::wl_data_device_manager_create_data_source(*display_, data_manager_->id);
  client::Proxy* source = display_->find_proxy(id);
  if (source == nullptr) throw std::runtime_error("could not create a clipboard source");
  for (const char* type : text_types) {
    if (control) generated::zwlr_data_control_source_v1_offer(*display_, id, type);
    else generated::wl_data_source_offer(*display_, id, type);
  }
  const SourceEvents events{&dead_sources_, generation, control};
  if (control) {
    source->set_observer(generated::zwlr_data_control_source_v1_observer(events));
    generated::zwlr_data_control_device_v1_set_selection(*display_, control_device_->id, source);
  } else {
    source->set_observer(generated::wl_data_source_observer(events));
    generated::wl_data_device_set_selection(*display_, data_device_->id, source, input_serial_);
  }
  if (!display_->flush()) throw std::runtime_error("could not set the clipboard");
}

std::optional<std::string> WaylandWidget::paste_from_clipboard() {
  if (clipboard.owned) return clipboard.text;
  client::Proxy* offer = control_offer_ != nullptr ? control_offer_ : selection_offer_;
  if (offer == nullptr) return std::nullopt;
  const auto& offered = offer_types_[offer->id];
  const char* type = nullptr;
  for (const char* candidate : text_types) {
    if (std::ranges::find(offered, candidate) != offered.end()) {
      type = candidate;
      break;
    }
  }
  if (type == nullptr) return std::nullopt;
  std::array<int, 2> ends{};
  if (pipe2(ends.data(), O_CLOEXEC | O_NONBLOCK) < 0) return std::nullopt;
  const UniqueFd read_end(ends[0]);
  {
    // The request carries its own duplicate; ours must close so EOF arrives.
    const UniqueFd write_end(ends[1]);
    if (offer == control_offer_)
      generated::zwlr_data_control_offer_v1_receive(*display_, offer->id, type, write_end.get());
    else generated::wl_data_offer_receive(*display_, offer->id, type, write_end.get());
  }
  if (!display_->flush()) throw std::runtime_error("could not request clipboard contents");
  std::string text;
  std::array<char, 4096> chunk{};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (text.size() < (1U << 20U)) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) break;
    pollfd ready{read_end.get(), POLLIN, 0};
    if (poll(&ready, 1, static_cast<int>(left)) <= 0) break;
    const ssize_t count = read(read_end.get(), chunk.data(), chunk.size());
    if (count == 0) break;
    if (count < 0) {
      if (errno == EAGAIN || errno == EINTR) continue;
      break;
    }
    text.append(chunk.data(), static_cast<std::size_t>(count));
  }
  return text;
}

void WaylandWidget::create_keyboard() {
  if (keyboard_ != nullptr) return;
  const std::uint32_t keyboard_id = generated::wl_seat_get_keyboard(*display_, seat_->id);
  keyboard_ = display_->find_proxy(keyboard_id);
  if (keyboard_ == nullptr) throw std::runtime_error("could not create wl_keyboard");
  keyboard_state_ = std::make_unique<KeyboardState>();
  keyboard_->set_observer([this](client::Proxy&, std::uint32_t opcode,
                                 wire::MessageParser& parser) {
    if (opcode == generated::ZWAYLAND_WL_KEYBOARD_EVENT_KEYMAP_OPCODE) {
      const std::uint32_t format = parser.read_uint();
      const int fd = parser.read_fd();
      const std::uint32_t size = parser.read_uint();
      void* mapping = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
      if (mapping != MAP_FAILED && format == generated::WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
        keyboard_state_->set_keymap(static_cast<const char*>(mapping));
      if (mapping != MAP_FAILED) munmap(mapping, size);
      close(fd);
    } else if (opcode == generated::ZWAYLAND_WL_KEYBOARD_EVENT_ENTER_OPCODE) {
      input_serial_ = parser.read_uint();
      clipboard_surface = this;
      const std::uint32_t surface_id = parser.read_object();
      (void)parser.read_array();
      role_->keyboard_enter(surface_id);
    } else if (opcode == generated::ZWAYLAND_WL_KEYBOARD_EVENT_LEAVE_OPCODE) {
      (void)parser.read_uint();
      const std::uint32_t surface_id = parser.read_object();
      role_->keyboard_leave(surface_id);
      stop_key_repeat();
    } else if (opcode == generated::ZWAYLAND_WL_KEYBOARD_EVENT_KEY_OPCODE) {
      const std::uint32_t serial = parser.read_uint();
      (void)parser.read_uint();
      const std::uint32_t key = parser.read_uint();
      const std::uint32_t state = parser.read_uint();
      if (state == generated::WL_KEYBOARD_KEY_STATE_PRESSED) {
        input_serial_ = serial;
        clipboard_surface = this;
        repeat_key_ = 0;
        if (repeat_rate_ > 0 && keyboard_state_->keymap != nullptr &&
            xkb_keymap_key_repeats(keyboard_state_->keymap, key + 8U)) {
          repeat_key_ = key;
          repeat_at_ = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(repeat_delay_);
        }
        keyboard_key(key, false);
      } else if (key == repeat_key_) {
        stop_key_repeat();
      }
    } else if (opcode == generated::ZWAYLAND_WL_KEYBOARD_EVENT_REPEAT_INFO_OPCODE) {
      repeat_rate_ = std::max(0, parser.read_int());
      repeat_delay_ = std::max(0, parser.read_int());
    } else if (opcode == generated::ZWAYLAND_WL_KEYBOARD_EVENT_MODIFIERS_OPCODE) {
      input_serial_ = parser.read_uint();
      const std::uint32_t depressed = parser.read_uint();
      const std::uint32_t latched = parser.read_uint();
      const std::uint32_t locked = parser.read_uint();
      const std::uint32_t group = parser.read_uint();
      if (keyboard_state_->state != nullptr)
        xkb_state_update_mask(keyboard_state_->state, depressed, latched, locked, 0, 0, group);
    } else {
      parser.discard(generated::wl_keyboard_interface.events[opcode].signature);
    }
  });
}

void WaylandWidget::create_pointer() {
  if (pointer_ != nullptr) return;
  const std::uint32_t pointer_id = generated::wl_seat_get_pointer(*display_, seat_->id);
  pointer_ = display_->find_proxy(pointer_id);
  if (pointer_ == nullptr) throw std::runtime_error("could not create wl_pointer");
  if (cursor_shape_manager_ != nullptr) {
    const std::uint32_t device = generated::wp_cursor_shape_manager_v1_get_pointer(
        *display_, cursor_shape_manager_->id, pointer_);
    cursor_device_ = display_->find_proxy(device);
  }
  struct PointerEvents {
    WaylandWidget* widget;
    double vertical = 0.0;
    std::int32_t vertical_120 = 0;
    bool has_120 = false;
    void enter(client::Proxy&, std::uint32_t serial, client::Proxy* surface, double x, double y) {
      if (surface != nullptr) widget->role_->pointer_enter(surface->id);
      // Every surface of ours shows a cursor, lock surfaces included.
      widget->pointer_serial_ = serial;
      widget->cursor_shape_ = 0;
      if (surface != widget->surface_) {
        widget->set_cursor(generated::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
        return;
      }
      widget->pointer_inside_ = true;
      widget->pointer_motion(x, y);
    }
    void leave(client::Proxy&, std::uint32_t, client::Proxy* surface) {
      if (surface == widget->surface_)
        widget->pointer_leave();
    }
    void motion(client::Proxy&, std::uint32_t, double x, double y) {
      if (widget->pointer_inside_ || widget->dragging_ || !widget->text_drag_id_.empty())
        widget->pointer_motion(x, y);
    }
    void button(client::Proxy&, std::uint32_t serial, std::uint32_t, std::uint32_t button,
                 std::uint32_t state) {
      if (state == generated::WL_POINTER_BUTTON_STATE_PRESSED) {
        widget->input_serial_ = serial;
        clipboard_surface = widget;
      }
      if (widget->pointer_inside_ || widget->dragging_ || !widget->text_drag_id_.empty())
        widget->pointer_button(button, state);
    }
    void axis(client::Proxy& proxy, std::uint32_t, std::uint32_t axis, double value) {
      if (axis != generated::WL_POINTER_AXIS_VERTICAL_SCROLL) return;
      if (proxy.version < 5) widget->pointer_axis(value);
      else vertical += value;
    }
    void frame(client::Proxy&) {
      if (has_120) widget->pointer_axis(static_cast<double>(vertical_120) / 120.0 * 40.0);
      else if (vertical != 0.0) widget->pointer_axis(vertical);
      vertical = 0.0;
      vertical_120 = 0;
      has_120 = false;
    }
    void axis_source(client::Proxy&, std::uint32_t) {}
    void axis_stop(client::Proxy&, std::uint32_t, std::uint32_t) {}
    void axis_discrete(client::Proxy&, std::uint32_t, std::int32_t) {}
    void axis_value120(client::Proxy&, std::uint32_t axis, std::int32_t value120) {
      if (axis == generated::WL_POINTER_AXIS_VERTICAL_SCROLL) {
        vertical_120 += value120;
        has_120 = true;
      }
    }
    void axis_relative_direction(client::Proxy&, std::uint32_t, std::uint32_t) {}
    void warp(client::Proxy&, double x, double y) {
      if (widget->pointer_inside_) widget->pointer_motion(x, y);
    }

  };
  pointer_->set_observer(generated::wl_pointer_observer(PointerEvents{this}));
}

void WaylandWidget::create_buffer(const Frame& frame) {
  create_buffer_for(frame, buffer_);
}

void WaylandWidget::create_buffer_for(const Frame& frame, client::Proxy*& target) {
  using namespace zwayland;
  if (target != nullptr) {
    generated::wl_buffer_destroy(*display_, target->id);
    target = nullptr;
  }
  const std::uint32_t params_id =
      generated::zwp_linux_dmabuf_v1_create_params(*display_, dmabuf_->id);
  client::Proxy* params = display_->find_proxy(params_id);
  if (params == nullptr) throw std::runtime_error("could not create linux-dmabuf params");
  generated::zwp_linux_buffer_params_v1_add(
      *display_, params->id, frame.fd, 0, 0, frame.stride,
      static_cast<std::uint32_t>(frame.modifier >> 32U),
      static_cast<std::uint32_t>(frame.modifier));
  constexpr std::uint32_t drm_format_abgr8888 = 0x34324241U;
  const std::uint32_t buffer_id = generated::zwp_linux_buffer_params_v1_create_immed(
      *display_, params->id, static_cast<std::int32_t>(frame.width),
      static_cast<std::int32_t>(frame.height), drm_format_abgr8888, 0);
  target = display_->find_proxy(buffer_id);
  generated::zwp_linux_buffer_params_v1_destroy(*display_, params->id);
  if (target == nullptr) throw std::runtime_error("could not create wl_buffer");
}

void WaylandWidget::configure_surface_input(const Scene& scene) {
  if (scene.accepts_input && !scene.selective_input) {
    generated::wl_surface_set_input_region(*display_, surface_->id, nullptr);
    return;
  }
  const std::uint32_t region_id = generated::wl_compositor_create_region(*display_, compositor_->id);
  client::Proxy* region = display_->find_proxy(region_id);
  if (region == nullptr) throw std::runtime_error("could not create empty input region");
  if (scene.accepts_input && scene.selective_input) {
    for (const auto& bounds : scene.input_regions)
      generated::wl_region_add(*display_, region->id, bounds[0], bounds[1], bounds[2], bounds[3]);
  }
  generated::wl_surface_set_input_region(*display_, surface_->id, region);
  generated::wl_region_destroy(*display_, region->id);
}

void WaylandWidget::create_focus_grab() {
  if (focus_grab_manager_ == nullptr || focus_grab_ != nullptr || !role_->focus_grab()) return;
  const auto id = generated::zwwm_focus_grab_manager_v1_get_grab(
      *display_, focus_grab_manager_->id, surface_);
  focus_grab_ = display_->find_proxy(id);
  if (focus_grab_ == nullptr) throw std::runtime_error("could not create zwwm focus grab");
  struct Events {
    WaylandWidget* widget;
    void activated(client::Proxy&) {
      set_focus_grab_state(true, true);
      widget->focus_events_.push_back(PendingFocus{true, 0});
    }
    void dismissed(client::Proxy&, std::uint32_t reason) {
      set_focus_grab_state(true, false);
      // Keep the desired-state latch. Rebuilding a dismissed panel must not
      // repeatedly steal keyboard focus back from the new owner.
      widget->focus_events_.push_back(PendingFocus{false, reason});
    }
  };
  focus_grab_->set_observer(generated::zwwm_focus_grab_v1_observer(Events{this}));
}

void WaylandWidget::update_focus_grab() {
  if (focus_grab_ == nullptr || !configured_ || scene_.focus_grab == focus_grab_requested_) return;
  if (scene_.focus_grab) generated::zwwm_focus_grab_v1_activate(*display_, focus_grab_->id);
  else generated::zwwm_focus_grab_v1_release(*display_, focus_grab_->id);
  focus_grab_requested_ = scene_.focus_grab;
}

bool WaylandWidget::dispatch_focus_events() {
  if (focus_events_.empty()) return false;
  const auto events = std::exchange(focus_events_, {});
  for (const auto& event : events) {
    const FocusHandler handler = event.activated ? scene_.on_focus_activated
                                                 : scene_.on_focus_dismissed;
    invoke(handler, "focus-grab", vw::FocusEvent{event.activated, event.reason});
  }
  return true;
}

void WaylandWidget::pointer_motion(double x, double y) {
  pointer_x_ = x;
  pointer_y_ = y;
  if (dragging_ && (x != drag_previous_x_ || y != drag_previous_y_)) {
    dispatch_drag(drag_move_);
    drag_previous_x_ = x;
    drag_previous_y_ = y;
    drag_refresh_pending_ = true;
  }
  // Extend a mouse selection; the redraw waits for the next frame callback.
  if (!text_drag_id_.empty()) {
    for (const TextField& field : scene_.text_fields) {
      if (field.id != text_drag_id_) continue;
      const std::size_t caret =
          caret_at(field, scene_.interactions[field.interaction], x);
      if (caret != field.state->caret) {
        field.state->caret = caret;
        drag_refresh_pending_ = true;
      }
      break;
    }
  }
  int next = -1;
  for (std::size_t interaction_index = scene_.interactions.size();
       pointer_inside_ && interaction_index > 0; --interaction_index) {
    const Interaction& interaction = scene_.interactions[interaction_index - 1];
    if (!interaction.enabled) continue;
    if (interaction_contains(interaction, x, y)) {
      next = static_cast<int>(interaction_index - 1);
      break;
    }
  }
  // The cursor says what is under it: a hand over what clicks or drags, a
  // text cursor over a text field, the arrow elsewhere.
  if (pointer_inside_) {
    std::uint32_t shape = generated::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    const int under = dragging_ ? pressed_ : next;
    if (!text_drag_id_.empty()) {
      shape = generated::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
    } else if (under >= 0 && static_cast<std::size_t>(under) < scene_.interactions.size()) {
      const Interaction& interaction = scene_.interactions[static_cast<std::size_t>(under)];
      if (interaction.text_field >= 0) shape = generated::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
      else if (interaction.on_click || interaction.on_drag_start || interaction.on_drag)
        shape = generated::WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
    }
    set_cursor(shape);
  }
  // Passive hover handlers track containment, not the topmost widget.
  std::vector<std::pair<HoverHandler, bool>> crossings;
  for (Interaction& interaction : scene_.interactions) {
    if (!interaction.on_hover) continue;
    const bool inside = pointer_inside_ && interaction_contains(interaction, x, y);
    if (inside == interaction.pointer_inside) continue;
    interaction.pointer_inside = inside;
    crossings.emplace_back(interaction.on_hover, inside);
  }
  // A drag keeps the widget it started on hovered; what it passes over stays
  // still.
  if (next != hovered_ && !dragging_) {
    const int previous = hovered_;
    hovered_ = next;
    if (previous >= 0) {
      auto& interaction = scene_.interactions[static_cast<std::size_t>(previous)];
      interaction.hovered = false;
      interaction.hover_target = 0.0F;
    }
    if (next >= 0) {
      auto& interaction = scene_.interactions[static_cast<std::size_t>(next)];
      interaction.hovered = true;
      interaction.hover_target = 1.0F;
    }
  }
  if (crossings.empty()) return;
  for (const auto& [handler, inside] : crossings) invoke(handler, "hover", inside);
  refresh_scene();
}

void WaylandWidget::set_cursor(std::uint32_t shape) {
  if (cursor_device_ == nullptr || pointer_serial_ == 0 || shape == cursor_shape_) return;
  cursor_shape_ = shape;
  generated::wp_cursor_shape_device_v1_set_shape(*display_, cursor_device_->id, pointer_serial_, shape);
}

void WaylandWidget::pointer_leave() {
  pointer_inside_ = false;
  if (!dragging_) pressed_ = -1;
  pointer_motion(pointer_x_, pointer_y_);
}

void WaylandWidget::pointer_button(std::uint32_t button, std::uint32_t state) {
  pointer_motion(pointer_x_, pointer_y_);
  if (state == zwayland::generated::WL_POINTER_BUTTON_STATE_PRESSED) {
    if (dragging_) return;
    pressed_ = hovered_;
    if (pressed_ >= 0) {
      const Interaction& interaction = scene_.interactions[static_cast<std::size_t>(pressed_)];
      if (interaction.text_field >= 0 && button == 272) {  // BTN_LEFT
        text_pointer_press(static_cast<std::size_t>(pressed_));
        return;
      }
      if (button == interaction.drag_button && interaction.draggable()) {
        // Copy the handlers: rebuilds during the drag replace scene_.
        dragging_ = true;
        drag_button_ = button;
        drag_move_ = interaction.on_drag;
        drag_end_ = interaction.on_drag_end;
        drag_start_x_ = drag_previous_x_ = pointer_x_;
        drag_start_y_ = drag_previous_y_ = pointer_y_;
        dispatch_drag(interaction.on_drag_start);
        drag_refresh_pending_ = true;
      }
    }
    return;
  }
  if (state != zwayland::generated::WL_POINTER_BUTTON_STATE_RELEASED) return;
  if (dragging_) {
    if (button == drag_button_) end_drag(false);
    return;
  }
  if (!text_drag_id_.empty()) {
    text_drag_id_.clear();
    pressed_ = -1;
    refresh_scene();
    return;
  }
  const int activated = pressed_;
  pressed_ = -1;
  if (activated < 0 || activated != hovered_) return;
  Interaction& interaction = scene_.interactions[static_cast<std::size_t>(activated)];
  // Text fields act on press (text_pointer_press); other buttons do nothing.
  if (interaction.text_field >= 0) return;
  if (!interaction.on_click) return;
  const ClickHandler handler = interaction.on_click;
  invoke(handler, "click", ClickEvent{pointer_x_, pointer_y_, button});
  refresh_scene();
}

std::size_t WaylandWidget::caret_at(const TextField& field, const Interaction& interaction,
                                    double x) const {
  const double local = x - (interaction.x + field.layout.padding) + field.state->scroll;
  std::size_t nearest = field.state->caret;
  double best = std::numeric_limits<double>::infinity();
  for (const auto& [offset, stop] : field.layout.stops) {
    if (std::abs(stop - local) >= best) continue;
    best = std::abs(stop - local);
    nearest = offset;
  }
  return nearest;
}

void WaylandWidget::text_pointer_press(std::size_t interaction_index) {
  const Interaction& interaction = scene_.interactions[interaction_index];
  const TextField& field = scene_.text_fields[static_cast<std::size_t>(interaction.text_field)];
  for (TextField& other : scene_.text_fields) {
    if (other.state == field.state) continue;
    other.state->focused = false;
    other.state->anchor.reset();
  }
  TextFieldState& state = *field.state;
  state.focused = true;
  const std::size_t offset = caret_at(field, interaction, pointer_x_);
  // One click places the caret and starts a drag selection, two select a
  // word, three select everything.
  const auto now = std::chrono::steady_clock::now();
  const bool repeated = field.id == last_click_id_ && now - last_click_ < std::chrono::milliseconds(400);
  click_count_ = repeated ? click_count_ % 3 + 1 : 1;
  last_click_ = now;
  last_click_id_ = field.id;
  const std::string& value = state.value;
  if (click_count_ == 1) {
    state.caret = offset;
    state.anchor = offset;
    text_drag_id_ = field.id;
  } else if (click_count_ == 2 && !field.password) {
    const auto space = [&value](std::size_t index) { return value[index] == ' ' || value[index] == '\t'; };
    std::size_t start = std::min(offset, value.size());
    std::size_t end = start;
    while (start > 0 && !space(start - 1)) --start;
    while (end < value.size() && !space(end)) ++end;
    state.anchor = start;
    state.caret = end;
  } else {
    state.anchor = 0;
    state.caret = value.size();
  }
  refresh_scene();
}

void WaylandWidget::dispatch_drag(const DragHandler& handler, bool cancelled) {
  if (!dragging_) return;
  invoke(handler, "drag", DragEvent{pointer_x_, pointer_y_, drag_start_x_, drag_start_y_,
                                    pointer_x_ - drag_start_x_, pointer_y_ - drag_start_y_,
                                    pointer_x_ - drag_previous_x_, pointer_y_ - drag_previous_y_,
                                    drag_button_, cancelled});
}

void WaylandWidget::end_drag(bool cancelled) {
  const DragHandler handler = std::exchange(drag_end_, {});
  dispatch_drag(handler, cancelled);
  dragging_ = false;
  drag_move_ = {};
  drag_refresh_pending_ = false;
  pressed_ = -1;
  refresh_scene();
}

void WaylandWidget::pointer_axis(double vertical) {
  if (!pointer_inside_ || vertical == 0.0 || !std::isfinite(vertical)) return;
  // The topmost widget with a scroll handler takes the wheel before scroll areas.
  for (std::size_t index = scene_.interactions.size(); index > 0; --index) {
    const Interaction& interaction = scene_.interactions[index - 1];
    if (!interaction.enabled || !interaction.on_scroll) continue;
    const auto& clip = interaction.clip;
    if (clip[2] > 0.0F && clip[3] > 0.0F &&
        (pointer_x_ < clip[0] || pointer_x_ >= clip[0] + clip[2] ||
         pointer_y_ < clip[1] || pointer_y_ >= clip[1] + clip[3])) continue;
    if (pointer_x_ < interaction.x || pointer_x_ >= interaction.x + interaction.width ||
        pointer_y_ < interaction.y || pointer_y_ >= interaction.y + interaction.height) continue;
    const ScrollHandler handler = interaction.on_scroll;
    invoke(handler, "scroll", ScrollEvent{pointer_x_, pointer_y_, vertical});
    refresh_scene();
    return;
  }
  for (auto area = scene_.scroll_areas.begin(); area != scene_.scroll_areas.end(); ++area) {
    if (!area->enabled) continue;
    if (pointer_x_ < area->x || pointer_x_ >= area->x + area->width ||
        pointer_y_ < area->y || pointer_y_ >= area->y + area->height) continue;
    const float previous = area->state->offset;
    area->state->offset = std::clamp(previous +
        static_cast<float>(vertical * area->step / 40.0), 0.0F, area->maximum);
    if (previous != area->state->offset) {
      hovered_ = -1;
      refresh_scene();
      pointer_motion(pointer_x_, pointer_y_);
    }
    return;
  }
}

void WaylandWidget::refresh_scene(bool input) {
  if (renderer_ == nullptr || source_ == nullptr) return;
  if (input) seen_epoch_ = ++global_input_epoch;
  try {
    if (role_->refresh()) return;
    set_surface_size({configured_width_, configured_height_});
    Scene next = source_->build();
    // A layer surface stays on its output; another output is a new surface.
    if (!next.mapped || roles::kind(next) != roles::kind(scene_) || next.output != scene_.output) {
      retired_ = true;
      return;
    }
    // Rebuilds often change nothing visible (a service event, a clock that
    // has not ticked over): adopt the scene for its handlers, skip the frame.
    if (configured_ && same_presentation(next, scene_)) {
      reload(next, Frame(0, 0, -1, 0, 0));
      return;
    }
    Frame frame = renderer_->render(next);
    reload(next, frame);
  } catch (const std::exception& error) {
    std::cerr << "vw: input refresh failed: " << error.what() << '\n';
  }
}

void WaylandWidget::keyboard_key(std::uint32_t key, bool repeat) {
  if (keyboard_state_ == nullptr || keyboard_state_->state == nullptr) return;
  xkb_state* xkb = keyboard_state_->state;
  const xkb_keysym_t symbol = xkb_state_key_get_one_sym(xkb, key + 8U);
  if (dragging_ && symbol == XKB_KEY_Escape) {
    if (!repeat) end_drag(true);
    return;
  }
  if (scene_.on_key) {
    KeyEvent event;
    event.keysym = symbol;
    std::array<char, 64> buffer{};
    if (xkb_keysym_get_name(symbol, buffer.data(), buffer.size()) > 0) event.name = buffer.data();
    if (const int length = xkb_state_key_get_utf8(xkb, key + 8U, buffer.data(), buffer.size());
        length > 0 && static_cast<unsigned char>(buffer[0]) >= 0x20U && buffer[0] != 0x7f)
      event.text.assign(buffer.data(), static_cast<std::size_t>(length));
    const auto active = [xkb](const char* modifier) {
      return xkb_state_mod_name_is_active(xkb, modifier, XKB_STATE_MODS_EFFECTIVE) > 0;
    };
    event.ctrl = active(XKB_MOD_NAME_CTRL);
    event.alt = active(XKB_MOD_NAME_ALT);
    event.shift = active(XKB_MOD_NAME_SHIFT);
    event.super = active(XKB_MOD_NAME_LOGO);
    event.repeat = repeat;
    // Copy: the handler may lead to scene_ being replaced.
    const KeyHandler handler = scene_.on_key;
    bool handled = false;
    try {
      handled = handler(event);
    } catch (const std::exception& error) {
      std::cerr << "vw: key callback failed: " << error.what() << '\n';
    }
    if (handled) {
      refresh_scene();
      return;
    }
  }
  auto field = std::ranges::find_if(scene_.text_fields,
      [](const TextField& candidate) { return candidate.state->focused; });
  if (field == scene_.text_fields.end()) return;
  TextFieldState& state = *field->state;
  std::string& value = state.value;
  const auto active = [xkb](const char* modifier) {
    return xkb_state_mod_name_is_active(xkb, modifier, XKB_STATE_MODS_EFFECTIVE) > 0;
  };
  const bool ctrl = active(XKB_MOD_NAME_CTRL);
  const bool alt = active(XKB_MOD_NAME_ALT);
  const bool shift = active(XKB_MOD_NAME_SHIFT);
  const auto continuation = [&value](std::size_t offset) {
    return offset < value.size() && (static_cast<unsigned char>(value[offset]) & 0xC0U) == 0x80U;
  };
  const auto previous = [&](std::size_t offset) {
    if (offset == 0) return offset;
    --offset;
    while (offset > 0 && continuation(offset)) --offset;
    return offset;
  };
  const auto next = [&](std::size_t offset) {
    if (offset >= value.size()) return value.size();
    ++offset;
    while (continuation(offset)) ++offset;
    return offset;
  };
  const auto space = [&value](std::size_t offset) {
    return value[offset] == ' ' || value[offset] == '\t';
  };
  // Word motion skips the spaces next to the caret, then the word itself.
  const auto word_left = [&](std::size_t offset) {
    while (offset > 0 && space(previous(offset))) offset = previous(offset);
    while (offset > 0 && !space(previous(offset))) offset = previous(offset);
    return offset;
  };
  const auto word_right = [&](std::size_t offset) {
    while (offset < value.size() && space(offset)) offset = next(offset);
    while (offset < value.size() && !space(offset)) offset = next(offset);
    return offset;
  };
  const auto wipe = [&] {
    if (field->authenticate && !value.empty()) explicit_bzero(value.data(), value.size());
    value.clear();
    state.caret = 0;
    state.anchor.reset();
  };
  // Removes the selection if there is one; returns whether it did.
  const auto erase_selection = [&] {
    if (!state.has_selection()) return false;
    const std::size_t start = state.selection_start();
    if (start == 0 && state.selection_end() == value.size()) {
      wipe();
      return true;
    }
    value.erase(start, state.selection_end() - start);
    state.caret = start;
    state.anchor.reset();
    return true;
  };
  // Inserts text at the caret, replacing the selection, within max_length.
  const auto insert = [&](std::string_view text) {
    const bool erased = erase_selection();
    std::size_t room = field->max_length > value.size() ? field->max_length - value.size() : 0;
    if (text.size() > room) {
      while (room > 0 && (static_cast<unsigned char>(text[room]) & 0xC0U) == 0x80U) --room;
      text = text.substr(0, room);
    }
    value.insert(state.caret, text);
    state.caret += text.size();
    return erased || !text.empty();
  };
  // Moves the caret; Shift extends the selection, otherwise motion first
  // collapses an existing selection.
  const auto move = [&](std::size_t to) {
    if (shift) {
      if (!state.anchor) state.anchor = state.caret;
    } else {
      state.anchor.reset();
    }
    state.caret = to;
  };
  const bool selecting = state.has_selection();

  bool changed = false;
  bool moved = false;
  if (symbol == XKB_KEY_BackSpace) {
    changed = erase_selection();
    if (!changed && state.caret > 0) {
      const std::size_t start = ctrl ? word_left(state.caret) : previous(state.caret);
      value.erase(start, state.caret - start);
      state.caret = start;
      changed = true;
    }
  } else if (symbol == XKB_KEY_Delete || symbol == XKB_KEY_KP_Delete) {
    changed = erase_selection();
    if (!changed && state.caret < value.size()) {
      const std::size_t end = ctrl ? word_right(state.caret) : next(state.caret);
      value.erase(state.caret, end - state.caret);
      changed = true;
    }
  } else if (symbol == XKB_KEY_Left || symbol == XKB_KEY_KP_Left) {
    move(!shift && selecting ? state.selection_start()
         : ctrl ? word_left(state.caret) : previous(state.caret));
    moved = true;
  } else if (symbol == XKB_KEY_Right || symbol == XKB_KEY_KP_Right) {
    move(!shift && selecting ? state.selection_end()
         : ctrl ? word_right(state.caret) : next(state.caret));
    moved = true;
  } else if (symbol == XKB_KEY_Home || symbol == XKB_KEY_KP_Home) {
    move(0);
    moved = true;
  } else if (symbol == XKB_KEY_End || symbol == XKB_KEY_KP_End) {
    move(value.size());
    moved = true;
  } else if (ctrl && (symbol == XKB_KEY_a || symbol == XKB_KEY_A)) {
    state.anchor = 0;
    state.caret = value.size();
    moved = true;
  } else if (ctrl && (symbol == XKB_KEY_c || symbol == XKB_KEY_C ||
                      symbol == XKB_KEY_x || symbol == XKB_KEY_X)) {
    // Passwords never reach the clipboard.
    if (selecting && !field->password && !repeat) {
      copy_to_clipboard(value.substr(state.selection_start(),
                                     state.selection_end() - state.selection_start()));
      if (symbol == XKB_KEY_x || symbol == XKB_KEY_X) changed = erase_selection();
    }
  } else if ((ctrl && (symbol == XKB_KEY_v || symbol == XKB_KEY_V)) ||
             (shift && symbol == XKB_KEY_Insert)) {
    if (const auto pasted = paste_from_clipboard()) {
      // One line: line breaks become spaces, other control characters go.
      std::string line;
      for (const char character : *pasted) {
        if (character == '\n' || character == '\r' || character == '\t') line += ' ';
        else if (static_cast<unsigned char>(character) >= 0x20U && character != 0x7f) line += character;
      }
      changed = insert(line);
    }
  } else if (ctrl && (symbol == XKB_KEY_u || symbol == XKB_KEY_U)) {
    if (state.caret > 0) {
      if (field->authenticate) explicit_bzero(value.data(), state.caret);
      value.erase(0, state.caret);
      state.caret = 0;
      state.anchor.reset();
      changed = true;
    }
  } else if (symbol == XKB_KEY_Tab || symbol == XKB_KEY_ISO_Left_Tab) {
    // Tab and Shift+Tab move focus between the scene's text fields.
    const std::size_t count = scene_.text_fields.size();
    const auto index = static_cast<std::size_t>(field - scene_.text_fields.begin());
    const bool backwards = symbol == XKB_KEY_ISO_Left_Tab || shift;
    if (count > 1) {
      state.focused = false;
      state.anchor.reset();
      TextFieldState& target = *scene_.text_fields[(index + (backwards ? count - 1 : 1)) % count].state;
      target.focused = true;
      target.caret = target.value.size();
      target.anchor.reset();
      moved = true;
    }
  } else if (symbol == XKB_KEY_Return || symbol == XKB_KEY_KP_Enter) {
    if (repeat) return;
    if (field->authenticate) {
      const bool authenticated = session_authenticate(value);
      wipe();
      changed = true;
      invoke(field->on_authenticated, "authentication", authenticated);
    } else {
      invoke(field->on_submit, "submit", value);
    }
  } else if (symbol == XKB_KEY_Escape) {
    if (field->authenticate) {
      wipe();
    } else if (selecting) {
      state.anchor.reset();
    } else {
      state.focused = false;
    }
    changed = true;
  } else if (!ctrl && !alt) {
    std::array<char, 64> text{};
    const int length = xkb_state_key_get_utf8(xkb, key + 8U, text.data(), text.size());
    if (length > 0 && static_cast<unsigned char>(text[0]) >= 0x20U && text[0] != 0x7f)
      changed = insert(std::string_view(text.data(), static_cast<std::size_t>(length)));
  }
  if (changed && !field->authenticate) invoke(field->on_change, "change", field->state->value);
  if (changed || moved) refresh_scene();
}

bool WaylandWidget::animate_hover(float elapsed_seconds) {
  bool changed = false;
  for (Interaction& interaction : scene_.interactions) {
    if (interaction.original_bounds.empty()) {
      for (std::size_t primitive = interaction.begin; primitive < interaction.end; ++primitive) {
        const Rectangle& rectangle = scene_.rectangles[primitive];
        interaction.original_bounds.push_back(
            {rectangle.x, rectangle.y, rectangle.width, rectangle.height});
      }
    }
    const float previous = interaction.hover_amount;
    const float step = interaction.duration <= 0.0F ? 1.0F : elapsed_seconds / interaction.duration;
    if (interaction.hover_amount < interaction.hover_target)
      interaction.hover_amount = std::min(interaction.hover_target, interaction.hover_amount + step);
    else if (interaction.hover_amount > interaction.hover_target)
      interaction.hover_amount = std::max(interaction.hover_target, interaction.hover_amount - step);
    if (interaction.hover_amount == previous && elapsed_seconds > 0.0F) continue;
    changed = changed || interaction.hover_amount != previous;
    const float eased = 1.0F - std::pow(1.0F - interaction.hover_amount, 3.0F);
    for (std::size_t primitive = interaction.begin; primitive < interaction.end; ++primitive) {
      Rectangle& rectangle = scene_.rectangles[primitive];
      const std::size_t local = primitive - interaction.begin;
      const auto& bounds = interaction.original_bounds[local];
      const float scale = 1.0F + (interaction.hover_scale - 1.0F) * eased;
      rectangle.width = bounds[2] * scale;
      rectangle.height = bounds[3] * scale;
      rectangle.x = bounds[0] + (bounds[2] - rectangle.width) * 0.5F;
      rectangle.y = bounds[1] + (bounds[3] - rectangle.height) * 0.5F;
      if (interaction.has_hover_color) for (std::size_t channel = 0; channel < 4; ++channel) {
        const float original = interaction.original_colors[local][channel];
        rectangle.color[channel] = original + (interaction.hover_color[channel] - original) * eased;
      }
    }
  }
  return changed;
}

bool WaylandWidget::wants_frame() const {
  return scene_.animating || drag_refresh_pending_ ||
         std::ranges::any_of(scene_.interactions, [](const Interaction& interaction) {
           return interaction.hover_amount != interaction.hover_target;
         });
}

void WaylandWidget::request_animation_frame() {
  if (closed_ || frame_callback_ != 0 || surface_ == nullptr ||
      !role_->ready()) return;
  if (!wants_frame()) {
    animation_last_tick_ = {};
    return;
  }
  // Normally the request already went out with the last buffer (see
  // add_frame_callback). Getting here means that frame committed nothing,
  // e.g. a rebuild with no visible change, so commit the request alone.
  add_frame_callback(surface_->id);
  generated::wl_surface_commit(*display_, surface_->id);
  if (!display_->flush()) throw std::runtime_error("could not request animation frame");
}

void WaylandWidget::add_frame_callback(std::uint32_t surface) {
  if (frame_callback_ != 0 || surface_ == nullptr || surface != surface_->id) return;
  if (animation_last_tick_ == std::chrono::steady_clock::time_point{})
    animation_last_tick_ = std::chrono::steady_clock::now();
  frame_callback_ = generated::wl_surface_frame(*display_, surface);
  auto* callback = display_->find_proxy(frame_callback_);
  if (callback == nullptr) throw std::runtime_error("could not create animation frame callback");
  callback->set_observer([this](client::Proxy& proxy, std::uint32_t opcode,
                                wire::MessageParser& parser) {
    if (opcode != 0) return;
    (void)parser.read_uint();
    frame_callback_ = 0;
    animation_frame_ready_ = true;
    display_->destroy_proxy(proxy.id);
  });
}

void WaylandWidget::reload(const Scene& scene, const Frame& frame) {
  if (roles::kind(scene) != roles::kind(scene_))
    throw std::runtime_error("cannot change Wayland surface role on reload");
  if (role_->reload(scene, frame)) return;
  const bool buffer_size_changed = frame.width != width_ || frame.height != height_;
  const Frame* next_frame = &frame;
  std::optional<Frame> retained_hover_frame;
  bool redraw_hover = false;
  if (&scene != &scene_) {
    const Scene previous = scene_;
    const int retained_hover = hovered_;
    scene_ = scene;
    // A rebuilt scene starts from where the pointer is now, without firing:
    // only pointer movement crosses hover boundaries.
    for (Interaction& interaction : scene_.interactions)
      if (interaction.on_hover)
        interaction.pointer_inside =
            pointer_inside_ && interaction_contains(interaction, pointer_x_, pointer_y_);
    hovered_ = retained_hover >= 0 &&
                       retained_hover < static_cast<int>(scene_.interactions.size())
                   ? retained_hover
                   : -1;
    if (hovered_ >= 0) {
      Interaction& interaction = scene_.interactions[static_cast<std::size_t>(hovered_)];
      interaction.hovered = true;
      interaction.hover_amount = previous.interactions[static_cast<std::size_t>(hovered_)].hover_amount;
      interaction.hover_target = previous.interactions[static_cast<std::size_t>(hovered_)].hover_target;
      (void)animate_hover(0.0F);
      // Only a visible hover needs painting over what was already rendered.
      redraw_hover = interaction.hover_amount > 0.0F &&
                     (interaction.has_hover_color || interaction.hover_scale != 1.0F);
    }
    if (same_presentation(scene_, previous)) return;
    if (redraw_hover && renderer_ != nullptr) {
      retained_hover_frame.emplace(renderer_->render(scene_));
      next_frame = &*retained_hover_frame;
    }
  }
  width_ = next_frame->width;
  height_ = next_frame->height;
  create_buffer(*next_frame);
  // Service events and animation ticks can arrive before the initial role
  // configure. Keep the newest wl_buffer ready, but never attach it before ack.
  if (!configured_) return;
  role_->present(buffer_size_changed);
}

namespace {

// Async-signal-safe self-pipe: handlers only write the signal number, and the
// event loop decides what to do with it. Child processes are unaffected
// because no signals are blocked.
int signal_pipe_write = -1;

void forward_signal(int number) {
  const int saved = errno;
  const auto byte = static_cast<unsigned char>(number);
  const ssize_t written = write(signal_pipe_write, &byte, 1);
  (void)written;
  errno = saved;
}

class SignalPipe {
 public:
  enum class Request { none, reload, quit };

  SignalPipe() {
    std::array<int, 2> ends{};
    if (pipe2(ends.data(), O_CLOEXEC | O_NONBLOCK) < 0)
      throw std::runtime_error("could not create signal pipe");
    read_.reset(ends[0]);
    write_.reset(ends[1]);
    signal_pipe_write = write_.get();
    struct sigaction action{};
    action.sa_handler = forward_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (std::size_t index = 0; index < handled.size(); ++index)
      sigaction(handled[index], &action, &previous_[index]);
  }

  ~SignalPipe() {
    for (std::size_t index = 0; index < handled.size(); ++index)
      sigaction(handled[index], &previous_[index], nullptr);
    signal_pipe_write = -1;
  }

  SignalPipe(const SignalPipe&) = delete;
  SignalPipe& operator=(const SignalPipe&) = delete;

  int fd() const { return read_.get(); }

  // SIGHUP requests a reload; SIGINT and SIGTERM request a clean exit.
  Request drain() const {
    Request request = Request::none;
    unsigned char number = 0;
    while (read(read_.get(), &number, 1) > 0) {
      if (number == SIGHUP) request = std::max(request, Request::reload);
      else request = Request::quit;
    }
    return request;
  }

 private:
  static constexpr std::array<int, 3> handled{SIGINT, SIGTERM, SIGHUP};
  UniqueFd read_;
  UniqueFd write_;
  std::array<struct sigaction, 3> previous_{};
};

}  // namespace

void WaylandWidget::attach(SceneSource& source, Renderer& renderer) {
  source_ = &source;
  renderer_ = &renderer;
}

int WaylandWidget::fd() const { return display_->fd(); }

bool WaylandWidget::animating() const { return scene_.animating; }

void WaylandWidget::prepare() {
  if (role_->prepare()) return;
  request_animation_frame();
}

bool WaylandWidget::dispatch(short revents) {
  if ((revents & (POLLERR | POLLHUP)) != 0) return false;
  if ((revents & POLLIN) != 0 && !display_->dispatch_pending()) return false;
  if (!dead_sources_.empty()) {
    for (const auto& [id, control] : std::exchange(dead_sources_, {})) {
      if (control) generated::zwlr_data_control_source_v1_destroy(*display_, id);
      else generated::wl_data_source_destroy(*display_, id);
    }
    if (!display_->flush()) return false;
  }
  return true;
}

void WaylandWidget::tick(std::chrono::steady_clock::time_point now) {
  // A frame callback may never come when nothing was damaged (a drag step that
  // changed nothing commits no buffer): a drag must not wait on it.
  if (drag_refresh_pending_ && now - drag_frame_at_ >= drag_frame_limit) animation_frame_ready_ = true;
  if (!animation_frame_ready_ || renderer_ == nullptr) return;
  animation_frame_ready_ = false;
  const float elapsed = std::chrono::duration<float>(now - animation_last_tick_).count();
  animation_last_tick_ = now;
  const bool hover_changed = animate_hover(elapsed);
  if (drag_refresh_pending_) {
    drag_refresh_pending_ = false;
    drag_frame_at_ = now;
    refresh_scene(true);
  } else if (scene_.animating) {
    refresh_scene(false);
  } else if (hover_changed) {
    reload(scene_, renderer_->render(scene_));
  }
}

std::optional<std::chrono::steady_clock::time_point> WaylandWidget::deadline() const {
  std::optional<std::chrono::steady_clock::time_point> next;
  if (repeat_key_ != 0) next = repeat_at_;
  if (drag_refresh_pending_) {
    const auto drag = drag_frame_at_ + drag_frame_limit;
    next = next ? std::min(*next, drag) : drag;
  }
  return next;
}

void WaylandWidget::timers(std::chrono::steady_clock::time_point now) {
  if (repeat_key_ == 0 || now < repeat_at_) return;
  const auto period = std::chrono::microseconds(1000000 / std::max(1, repeat_rate_));
  // Fire once per wakeup and catch up the schedule, so a slow frame never
  // replays a burst of repeats.
  while (repeat_at_ <= now) repeat_at_ += period;
  keyboard_key(repeat_key_, true);
}

void WaylandWidget::throw_if_protocol_error() const {
  if (display_->has_error())
    throw std::runtime_error("Wayland protocol error: " + display_->error_message());
}

namespace {

struct Slot {
  SceneSource* source = nullptr;
  std::unique_ptr<WaylandWidget> widget;
  // Input epoch at which an unmapped surface was last built.
  std::uint64_t absent_epoch = 0;
  // Last periodic (or full) rebuild; each surface keeps its own
  // refresh_interval, so a slow surface is not rebuilt at a fast one's rate.
  std::chrono::steady_clock::time_point refreshed = std::chrono::steady_clock::now();

  // Unmapped surfaces are probed once a second, as before.
  double interval() const { return widget ? widget->scene().refresh_interval : 1.0; }
  std::chrono::steady_clock::time_point due() const {
    return refreshed + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                           std::chrono::duration<double>(interval()));
  }
};

// Creates the surface when its scene maps. Errors are reported per surface so
// one broken surface does not take down the others.
void materialize(Slot& slot, Renderer& renderer) {
  try {
    set_surface_size({});
    const Scene scene = slot.source->build();
    if (!scene.mapped) return;
    const Frame frame = roles::uses_widget_buffer(scene) ? renderer.render(scene)
                                                         : Frame(scene.width, scene.height, -1, 0, 0);
    slot.widget = std::make_unique<WaylandWidget>(scene, frame);
    slot.widget->attach(*slot.source, renderer);
    slot.widget->mark_epoch(global_input_epoch);
  } catch (const std::exception& error) {
    slot.widget.reset();
    std::cerr << "vw: could not create surface: " << error.what() << '\n';
  }
}

// Rebuilds every surface (or only those behind the input epoch), creating
// surfaces whose scenes have mapped since the last pass.
void synchronize(std::span<Slot> slots, Renderer& renderer, bool everything) {
  for (Slot& slot : slots) {
    if (!slot.widget) {
      if (!everything && slot.absent_epoch == global_input_epoch) continue;
      slot.absent_epoch = global_input_epoch;
      materialize(slot, renderer);
      continue;
    }
    if (!everything && slot.widget->input_epoch() == global_input_epoch) continue;
    slot.widget->refresh(false);
    slot.widget->mark_epoch(global_input_epoch);
  }
}

}  // namespace

void run_surfaces(SceneSource& shared, std::span<SceneSource* const> surfaces, Renderer& renderer) {
  const SignalPipe signals;
  UniqueFd notify;
  if (const auto directory = shared.watch_directory()) {
    notify.reset(inotify_init1(IN_CLOEXEC | IN_NONBLOCK));
    if (!notify) throw std::runtime_error("could not create file watcher");
    if (inotify_add_watch(notify.get(), directory->c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) < 0)
      throw std::runtime_error("could not watch " + directory->string());
  }
  const std::vector<SceneSource::EventSource> events = shared.event_sources();

  std::vector<Slot> slots;
  for (SceneSource* source : surfaces) slots.push_back({source, nullptr, global_input_epoch});
  // The first surface failing to start is fatal, matching single-surface use.
  for (Slot& slot : slots) {
    set_surface_size({});
    const Scene scene = slot.source->build();
    if (!scene.mapped) continue;
    const Frame frame = roles::uses_widget_buffer(scene) ? renderer.render(scene)
                                                         : Frame(scene.width, scene.height, -1, 0, 0);
    slot.widget = std::make_unique<WaylandWidget>(scene, frame);
    slot.widget->attach(*slot.source, renderer);
  }

  // signals, file watcher (-1 is ignored by poll), event sources, then one
  // display per live surface.
  enum : std::size_t { signal_fd, watch_fd, first_event_fd };
  const std::size_t first_display_fd = first_event_fd + events.size();
  std::vector<pollfd> fds;
  std::vector<Slot*> polled;

  while (!toplevel_close_requested()) {
    bool closing = false;
    for (Slot& slot : slots) {
      if (!slot.widget) continue;
      slot.widget->prepare();
      closing = closing || slot.widget->closed_by_compositor();
    }

    fds.assign({{signals.fd(), POLLIN, 0}, {notify.get(), POLLIN, 0}});
    for (const auto& event : events) fds.push_back({event.fd, POLLIN, 0});
    polled.clear();
    for (Slot& slot : slots) {
      if (!slot.widget) continue;
      fds.push_back({slot.widget->fd(), POLLIN, 0});
      polled.push_back(&slot);
    }

    const auto before_poll = std::chrono::steady_clock::now();
    auto wake_at = before_poll + std::chrono::hours(1);
    for (const Slot& slot : slots) wake_at = std::min(wake_at, slot.due());
    for (const Slot* slot : polled)
      if (const auto due = slot->widget->deadline()) wake_at = std::min(wake_at, *due);
    const double wait_ms = std::max(0.0, std::chrono::duration<double, std::milli>(wake_at - before_poll).count());
    // An unlock finished in prepare() is handled without waiting.
    const int timeout = closing ? 0 : static_cast<int>(std::ceil(wait_ms));
    if (poll(fds.data(), fds.size(), timeout) < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("widget event poll failed");
    }
    const auto readable = [&fds](std::size_t index) { return (fds[index].revents & POLLIN) != 0; };

    bool force_reload = false;
    if (readable(signal_fd)) {
      const SignalPipe::Request request = signals.drain();
      if (request == SignalPipe::Request::quit) {
        std::cerr << "vw: received termination signal, exiting\n";
        return;
      }
      force_reload = request == SignalPipe::Request::reload;
    }

    // Surface input. A dead connection on any surface ends the app.
    for (std::size_t index = 0; index < polled.size(); ++index) {
      WaylandWidget& widget = *polled[index]->widget;
      if (!widget.dispatch(fds[first_display_fd + index].revents)) {
        widget.throw_if_protocol_error();
        return;
      }
    }

    const auto woke = std::chrono::steady_clock::now();
    for (Slot* slot : polled) slot->widget->timers(woke);

    bool service_changed = false;
    for (Slot* slot : polled) service_changed = slot->widget->take_focus_events() || service_changed;
    for (std::size_t index = 0; index < events.size(); ++index) {
      if (!readable(first_event_fd + index)) continue;
      try {
        service_changed = events[index].drain() || service_changed;
      } catch (const std::exception& error) {
        std::cerr << "vw: event source failed: " << error.what() << '\n';
      }
    }

    // Closed and retired surfaces. Closing calls the scene's handler; with no
    // handler the app ends, as a single window or lock screen always did.
    bool closed_any = false;
    for (Slot& slot : slots) {
      if (!slot.widget) continue;
      if (slot.widget->closed_by_compositor()) {
        const CloseHandler handler = slot.widget->scene().on_closed;
        slot.widget.reset();
        if (!handler) return;
        try {
          handler();
        } catch (const std::exception& error) {
          std::cerr << "vw: close callback failed: " << error.what() << '\n';
        }
        closed_any = true;
      } else if (slot.widget->retired()) {
        slot.widget.reset();
        closed_any = true;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    bool changed = service_changed || force_reload || closed_any;
    alignas(inotify_event) std::array<char, 4096> buffer{};
    for (ssize_t count = 0; readable(watch_fd) &&
                        (count = read(notify.get(), buffer.data(), buffer.size())) > 0;) {
      for (ssize_t offset = 0; offset < count;) {
        const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
        if (event->len > 0 && shared.affects(event->name)) changed = true;
        offset += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
      }
    }
    if (changed) {
      for (Slot& slot : slots) slot.refreshed = now;
      synchronize(slots, renderer, true);
    } else {
      // Periodic data refreshes, per surface. They must not drive an
      // animation when a surface is occluded and the compositor has stopped
      // delivering frame callbacks: an animating surface redraws on its own.
      for (Slot& slot : slots) {
        if (now < slot.due()) continue;
        slot.refreshed = now;
        if (!slot.widget) {
          slot.absent_epoch = global_input_epoch;
          materialize(slot, renderer);
        } else if (!slot.widget->animating()) {
          slot.widget->refresh(false);
          slot.widget->mark_epoch(global_input_epoch);
        }
      }
      // Input on one surface changes state the others may draw.
      synchronize(slots, renderer, false);
    }

    for (Slot& slot : slots)
      if (slot.widget) slot.widget->tick(now);
  }
}

void set_clipboard(std::string_view text) {
  if (clipboard_surface != nullptr) clipboard_surface->copy_to_clipboard(std::string(text));
}

std::optional<std::string> clipboard_text() {
  if (clipboard.owned) return clipboard.text;
  if (clipboard_surface == nullptr) return std::nullopt;
  return clipboard_surface->paste_from_clipboard();
}

}  // namespace vw
