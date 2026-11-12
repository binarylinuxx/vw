#include "vw/capture.hpp"

#include <ext-zwwm-toplevels-unstable-v1-zwayland-client.h>
#include <zwwm-capture-access-v1-zwayland-client.h>
#include <zwwm-screencopy-view-unstable-v1-zwayland-client.h>
#include <wayland-zwayland-client.h>
#include <zwayland/client/core.hpp>
#include <zwayland/client/display.hpp>

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vw {
namespace {

namespace generated = zwayland::generated;
using zwayland::client::Proxy;

struct Output {
  Proxy* proxy = nullptr;
  std::string name;
};

// Converts one wl_shm pixel to RGBA with red in the low byte.
std::uint32_t to_rgba(std::uint32_t pixel, std::uint32_t format) {
  switch (format) {
    case generated::WL_SHM_FORMAT_ARGB8888:
    case generated::WL_SHM_FORMAT_XRGB8888: {
      const std::uint32_t alpha = format == generated::WL_SHM_FORMAT_ARGB8888 ? pixel >> 24U : 0xffU;
      return ((pixel >> 16U) & 0xffU) | (pixel & 0xff00U) | ((pixel & 0xffU) << 16U) | (alpha << 24U);
    }
    case generated::WL_SHM_FORMAT_XBGR8888: return pixel | 0xff000000U;
    default: return pixel;
  }
}

// zwwm writes the token at startup and names it in ZWWM_CAPTURE_TOKEN; a
// client started some other way finds it next to the Wayland socket.
std::vector<std::byte> read_capture_token() {
  std::string path;
  if (const char* named = std::getenv("ZWWM_CAPTURE_TOKEN"); named != nullptr && *named != '\0') {
    path = named;
  } else {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const char* socket = std::getenv("WAYLAND_DISPLAY");
    if (runtime == nullptr || socket == nullptr || *socket == '\0') return {};
    path = std::string(runtime) + "/" + socket + ".capture-token";
  }
  std::ifstream file(path, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (bytes.size() > 4096) return {};
  std::vector<std::byte> token(bytes.size());
  std::ranges::transform(bytes, token.begin(), [](char value) { return static_cast<std::byte>(value); });
  return token;
}

// What differs between wlr-screencopy frames and zwwm's: the opcodes of
// their otherwise matching events, and their requests.
struct FrameProtocol {
  std::uint32_t buffer;
  std::uint32_t flags;
  std::uint32_t ready;
  std::uint32_t failed;
  // wlr-screencopy version 3 ends the buffer list with buffer_done.
  std::optional<std::uint32_t> buffer_done;
  void (*copy)(zwayland::client::Display&, std::uint32_t, Proxy*);
  void (*destroy)(zwayland::client::Display&, std::uint32_t);
};

struct WindowEntry {
  Proxy* handle = nullptr;
  CaptureWindow info;
  bool closed = false;
};

class Capturer {
 public:
  Capturer() : display_(zwayland::client::Display::connect()) {
    if (!display_) throw std::runtime_error("could not connect to the Wayland compositor");
    Proxy* registry = zwayland::client::core::get_registry(*display_);
    if (registry == nullptr) throw std::runtime_error("could not create the Wayland registry");
    struct Registry {
      Capturer* self;
      Proxy* registry;
      void global(Proxy&, std::uint32_t name, const std::string& interface, std::uint32_t version) {
        auto& display = *self->display_;
        if (interface == generated::zwlr_screencopy_manager_v1_interface.name) {
          self->manager_ = zwayland::client::core::bind(display, *registry, name,
              generated::zwlr_screencopy_manager_v1_interface,
              std::min(version, generated::zwlr_screencopy_manager_v1_interface.version));
        } else if (interface == generated::wl_shm_interface.name) {
          self->shm_ = zwayland::client::core::bind(display, *registry, name,
                                                    generated::wl_shm_interface, 1);
          // Formats are known from the buffer event; ignore the list.
          if (self->shm_ != nullptr) self->shm_->set_observer(discard);
        } else if (interface == generated::wl_output_interface.name) {
          self->add_output(*registry, name, version);
        } else if (interface == generated::zwwm_capture_access_v1_interface.name) {
          self->access_name_ = name;
        }
      }
      void global_remove(Proxy&, std::uint32_t) {}
    };
    registry->set_observer(generated::wl_registry_observer(Registry{this, registry}));
    registry_ = registry;
    // Bind, then let the outputs report their names.
    check(display_->roundtrip());
    check(display_->roundtrip());
    unlock_windows();
  }

  int fd() const { return toplevels_ != nullptr ? display_->fd() : -1; }

  bool drain() {
    if (toplevels_ == nullptr) return false;
    check(display_->dispatch_pending());
    return std::exchange(windows_changed_, false);
  }

  bool windows_available() const { return toplevels_ != nullptr; }

  std::vector<CaptureWindow> windows() const {
    std::vector<CaptureWindow> result;
    for (const auto& entry : windows_)
      if (!entry->closed) result.push_back(entry->info);
    return result;
  }

  Bitmap capture_output(const CaptureProps& props) {
    if (manager_ == nullptr || shm_ == nullptr)
      throw std::runtime_error("compositor does not offer wlr-screencopy");
    const auto output = std::ranges::find_if(outputs_, [&](const auto& entry) {
      return props.output.empty() || entry->name == props.output;
    });
    if (output == outputs_.end())
      throw std::runtime_error("no output named " + std::string(props.output));
    const std::int32_t cursor = props.cursor ? 1 : 0;
    const std::uint32_t id = props.region
        ? generated::zwlr_screencopy_manager_v1_capture_output_region(
              *display_, manager_->id, cursor, (*output)->proxy, props.region->x, props.region->y,
              props.region->width, props.region->height)
        : generated::zwlr_screencopy_manager_v1_capture_output(*display_, manager_->id, cursor,
                                                              (*output)->proxy);
    static constexpr FrameProtocol wlr{
        generated::ZWAYLAND_ZWLR_SCREENCOPY_FRAME_V1_EVENT_BUFFER_OPCODE,
        generated::ZWAYLAND_ZWLR_SCREENCOPY_FRAME_V1_EVENT_FLAGS_OPCODE,
        generated::ZWAYLAND_ZWLR_SCREENCOPY_FRAME_V1_EVENT_READY_OPCODE,
        generated::ZWAYLAND_ZWLR_SCREENCOPY_FRAME_V1_EVENT_FAILED_OPCODE,
        generated::ZWAYLAND_ZWLR_SCREENCOPY_FRAME_V1_EVENT_BUFFER_DONE_OPCODE,
        generated::zwlr_screencopy_frame_v1_copy, generated::zwlr_screencopy_frame_v1_destroy};
    return capture(id, wlr);
  }

  Bitmap capture_window(std::uint64_t window, bool cursor) {
    if (toplevels_ == nullptr || shm_ == nullptr)
      throw std::runtime_error("zwwm did not grant window capture");
    const auto found = std::ranges::find_if(windows_, [window](const auto& entry) {
      return entry->info.id == window && !entry->closed;
    });
    if (found == windows_.end()) throw std::runtime_error("no such window");
    const std::uint32_t id =
        generated::ext_zwwm_toplevel_handle_v1_capture(*display_, (*found)->handle->id, cursor ? 1 : 0);
    static constexpr FrameProtocol zwwm{
        generated::ZWAYLAND_ZWWM_SCREENCOPY_FRAME_V1_EVENT_BUFFER_OPCODE,
        generated::ZWAYLAND_ZWWM_SCREENCOPY_FRAME_V1_EVENT_FLAGS_OPCODE,
        generated::ZWAYLAND_ZWWM_SCREENCOPY_FRAME_V1_EVENT_READY_OPCODE,
        generated::ZWAYLAND_ZWWM_SCREENCOPY_FRAME_V1_EVENT_FAILED_OPCODE,
        std::nullopt,
        generated::zwwm_screencopy_frame_v1_copy, generated::zwwm_screencopy_frame_v1_destroy};
    return capture(id, zwwm);
  }

 private:
  struct Frame {
    std::uint32_t format = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    bool has_buffer = false;
    bool buffer_done = false;
    bool y_invert = false;
    bool ready = false;
    bool failed = false;
  };

  struct Shm {
    explicit Shm(std::size_t bytes) : size(bytes) {
      fd = memfd_create("vw-capture", MFD_CLOEXEC);
      if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
        if (fd >= 0) close(fd);
        throw std::runtime_error("could not allocate capture memory");
      }
      data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (data == MAP_FAILED) {
        close(fd);
        throw std::runtime_error("could not map capture memory");
      }
    }
    ~Shm() {
      munmap(data, size);
      close(fd);
    }
    Shm(const Shm&) = delete;
    Shm& operator=(const Shm&) = delete;
    int fd = -1;
    void* data = nullptr;
    std::size_t size = 0;
  };

  static void discard(Proxy& proxy, std::uint32_t opcode, zwayland::wire::MessageParser& parser) {
    parser.discard(proxy.interface->events[opcode].signature);
  }

  // Presents the token when zwwm asks for one, then binds window capture
  // from a fresh registry, which lists the globals access unlocked.
  void unlock_windows() {
    if (access_name_ == 0 || shm_ == nullptr) return;
    const std::vector<std::byte> token = read_capture_token();
    if (token.empty()) return;
    Proxy* access = zwayland::client::core::bind(*display_, *registry_, access_name_,
                                                 generated::zwwm_capture_access_v1_interface, 1);
    if (access == nullptr) return;
    struct AccessEvents {
      int* result;
      void granted(Proxy&) { *result = 1; }
      void denied(Proxy&) { *result = -1; }
    };
    int result = 0;
    access->set_observer(generated::zwwm_capture_access_v1_observer(AccessEvents{&result}));
    generated::zwwm_capture_access_v1_authenticate(*display_, access->id, token);
    while (result == 0) check(display_->dispatch());
    generated::zwwm_capture_access_v1_destroy(*display_, access->id);
    if (result < 0) return;

    Proxy* unlocked = zwayland::client::core::get_registry(*display_);
    if (unlocked == nullptr) return;
    struct Registry {
      Capturer* self;
      Proxy* registry;
      void global(Proxy&, std::uint32_t name, const std::string& interface, std::uint32_t) {
        if (interface == generated::ext_zwwm_toplevels_v1_interface.name)
          self->bind_toplevels(*registry, name);
      }
      void global_remove(Proxy&, std::uint32_t) {}
    };
    unlocked->set_observer(generated::wl_registry_observer(Registry{this, unlocked}));
    check(display_->roundtrip());
    // The initial window batch ends with done.
    check(display_->roundtrip());
  }

  void bind_toplevels(Proxy& registry, std::uint32_t name) {
    toplevels_ = zwayland::client::core::bind(*display_, registry, name,
                                              generated::ext_zwwm_toplevels_v1_interface, 1);
    if (toplevels_ == nullptr) return;
    struct ManagerEvents {
      Capturer* self;
      void toplevel(Proxy&, Proxy* handle) { self->add_window(handle); }
      void done(Proxy&) {
        std::erase_if(self->windows_, [](const auto& entry) { return entry->closed; });
        self->windows_changed_ = true;
      }
      void finished(Proxy&) {
        self->toplevels_ = nullptr;
        self->windows_.clear();
        self->windows_changed_ = true;
      }
    };
    toplevels_->set_observer(generated::ext_zwwm_toplevels_v1_observer(ManagerEvents{this}));
  }

  void add_window(Proxy* handle) {
    auto entry = std::make_unique<WindowEntry>();
    entry->handle = handle;
    entry->info.id = ++next_window_;
    struct HandleEvents {
      Capturer* self;
      WindowEntry* entry;
      void title(Proxy&, const std::string& title) { entry->info.title = title; }
      void app_id(Proxy&, const std::string& app_id) { entry->info.app_id = app_id; }
      void geometry(Proxy&, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
        entry->info.x = x;
        entry->info.y = y;
        entry->info.width = width;
        entry->info.height = height;
      }
      void state(Proxy&, std::uint32_t state) {
        entry->info.maximized = (state & 1U) != 0;
        entry->info.fullscreen = (state & 2U) != 0;
        entry->info.activated = (state & 4U) != 0;
        entry->info.minimized = (state & 8U) != 0;
      }
      void output(Proxy&, Proxy* output) {
        const auto found = std::ranges::find_if(self->outputs_, [output](const auto& item) {
          return item->proxy == output;
        });
        entry->info.output = found == self->outputs_.end() ? std::string() : (*found)->name;
      }
      void closed(Proxy& proxy) {
        entry->closed = true;
        generated::ext_zwwm_toplevel_handle_v1_destroy(*self->display_, proxy.id);
      }
    };
    handle->set_observer(generated::ext_zwwm_toplevel_handle_v1_observer(HandleEvents{this, entry.get()}));
    windows_.push_back(std::move(entry));
  }

  Bitmap capture(std::uint32_t id, const FrameProtocol& protocol) {
    Proxy* proxy = display_->find_proxy(id);
    if (proxy == nullptr) throw std::runtime_error("could not create a capture frame");
    Frame state;
    proxy->set_observer([&state, &protocol](Proxy& frame, std::uint32_t opcode,
                                            zwayland::wire::MessageParser& parser) {
      if (opcode == protocol.buffer) {
        state.format = parser.read_uint();
        state.width = parser.read_uint();
        state.height = parser.read_uint();
        state.stride = parser.read_uint();
        state.has_buffer = true;
        return;
      }
      if (opcode == protocol.flags) {
        state.y_invert = (parser.read_uint() & 1U) != 0;  // y_invert in both protocols
        return;
      }
      parser.discard(frame.interface->events[opcode].signature);
      if (opcode == protocol.ready) state.ready = true;
      else if (opcode == protocol.failed) state.failed = true;
      else if (protocol.buffer_done && opcode == *protocol.buffer_done) state.buffer_done = true;
    });
    // zwwm frames and early wlr versions send the buffer event straight away.
    const bool announces_done = protocol.buffer_done && proxy->version >= 3;
    try {
      while (!state.failed && !(announces_done ? state.buffer_done : state.has_buffer))
        check(display_->dispatch());
      if (state.failed || !state.has_buffer) throw std::runtime_error("capture failed");
      if (state.width == 0 || state.height == 0 || state.width > 16384 || state.height > 16384 ||
          state.stride < state.width * 4)
        throw std::runtime_error("capture has an unusable size");
      const auto size = static_cast<std::size_t>(state.stride) * state.height;
      Shm shm(size);
      const auto pool = generated::wl_shm_create_pool(*display_, shm_->id, shm.fd,
                                                      static_cast<std::int32_t>(size));
      const auto buffer = generated::wl_shm_pool_create_buffer(
          *display_, pool, 0, static_cast<std::int32_t>(state.width),
          static_cast<std::int32_t>(state.height), static_cast<std::int32_t>(state.stride),
          state.format);
      protocol.copy(*display_, id, display_->find_proxy(buffer));
      while (!state.ready && !state.failed) check(display_->dispatch());
      generated::wl_buffer_destroy(*display_, buffer);
      generated::wl_shm_pool_destroy(*display_, pool);
      protocol.destroy(*display_, id);
      check(display_->flush());
      if (state.failed) throw std::runtime_error("capture failed");
      return convert(state, shm);
    } catch (...) {
      if (display_->find_proxy(id) != nullptr) {
        protocol.destroy(*display_, id);
        (void)display_->flush();
      }
      throw;
    }
  }

  static Bitmap convert(const Frame& frame, const Shm& shm) {
    switch (frame.format) {
      case generated::WL_SHM_FORMAT_ARGB8888:
      case generated::WL_SHM_FORMAT_XRGB8888:
      case generated::WL_SHM_FORMAT_ABGR8888:
      case generated::WL_SHM_FORMAT_XBGR8888: break;
      default: throw std::runtime_error("capture uses an unsupported pixel format");
    }
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(frame.width) * frame.height);
    const auto* bytes = static_cast<const unsigned char*>(shm.data);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
      const std::uint32_t row = frame.y_invert ? frame.height - 1 - y : y;
      const auto* source = reinterpret_cast<const std::uint32_t*>(bytes + static_cast<std::size_t>(row) * frame.stride);
      auto* target = pixels.data() + static_cast<std::size_t>(y) * frame.width;
      for (std::uint32_t x = 0; x < frame.width; ++x) target[x] = to_rgba(source[x], frame.format);
    }
    return {frame.width, frame.height,
            std::make_shared<const std::vector<std::uint32_t>>(std::move(pixels))};
  }

  void add_output(Proxy& registry, std::uint32_t name, std::uint32_t version) {
    auto output = std::make_unique<Output>();
    output->proxy = zwayland::client::core::bind(*display_, registry, name,
        generated::wl_output_interface, std::min(version, generated::wl_output_interface.version));
    if (output->proxy == nullptr) return;
    struct OutputEvents {
      Output* output;
      void geometry(Proxy&, std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                    const std::string&, const std::string&, std::int32_t) {}
      void mode(Proxy&, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) {}
      void done(Proxy&) {}
      void scale(Proxy&, std::int32_t) {}
      void name(Proxy&, const std::string& name) { output->name = name; }
      void description(Proxy&, const std::string&) {}
    };
    output->proxy->set_observer(generated::wl_output_observer(OutputEvents{output.get()}));
    outputs_.push_back(std::move(output));
  }

  void check(bool succeeded) const {
    if (!succeeded || display_->has_error())
      throw std::runtime_error(display_->has_error() ? display_->error_message()
                                                       : "capture connection failed");
  }

  std::unique_ptr<zwayland::client::Display> display_;
  Proxy* manager_ = nullptr;
  Proxy* shm_ = nullptr;
  Proxy* registry_ = nullptr;
  std::uint32_t access_name_ = 0;
  Proxy* toplevels_ = nullptr;
  std::vector<std::unique_ptr<Output>> outputs_;
  std::vector<std::unique_ptr<WindowEntry>> windows_;
  std::uint64_t next_window_ = 0;
  bool windows_changed_ = false;
};

std::mutex capturer_mutex;
std::unique_ptr<Capturer> capturer_instance;

Capturer& capturer() {
  if (!capturer_instance) capturer_instance = std::make_unique<Capturer>();
  return *capturer_instance;
}

// Runs `work` on the shared connection; a failure drops the connection so
// the next call starts over.
template <typename Work>
auto with_capturer(Work&& work) {
  std::lock_guard lock(capturer_mutex);
  try {
    return work(capturer());
  } catch (...) {
    capturer_instance.reset();
    throw;
  }
}

}  // namespace

Bitmap capture_output(const CaptureProps& props) {
  return with_capturer([&](Capturer& capturer) { return capturer.capture_output(props); });
}

bool window_capture_available() {
  try {
    return with_capturer([](Capturer& capturer) { return capturer.windows_available(); });
  } catch (...) {
    return false;
  }
}

std::vector<CaptureWindow> capture_windows() {
  return with_capturer([](Capturer& capturer) { return capturer.windows(); });
}

Bitmap capture_window(std::uint64_t id, bool cursor) {
  return with_capturer([&](Capturer& capturer) { return capturer.capture_window(id, cursor); });
}

int capture_event_fd() {
  try {
    return with_capturer([](Capturer& capturer) { return capturer.fd(); });
  } catch (...) {
    return -1;
  }
}

bool drain_capture_events() {
  try {
    return with_capturer([](Capturer& capturer) { return capturer.drain(); });
  } catch (...) {
    return false;
  }
}

}  // namespace vw
