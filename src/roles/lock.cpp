// ext-session-lock: one lock surface per output, each rendered at the size
// its output configures. Input follows whichever lock surface last received
// keyboard or pointer focus.
#include "role.hpp"

#include "vw/host.hpp"

#include <session-lock-zwayland-client.h>
#include <wayland-zwayland-client.h>
#include <zwayland/client/core.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vw::roles {
namespace {

using namespace zwayland;

struct LockOutput {
  std::uint32_t global_name = 0;
  std::string name;
  client::Proxy* output = nullptr;
  client::Proxy* surface = nullptr;
  client::Proxy* lock_surface = nullptr;
  client::Proxy* buffer = nullptr;
  Scene scene;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool configured = false;
};

class LockRole final : public Role {
 public:
  using Role::Role;

  void global(std::uint32_t name, std::string_view interface, std::uint32_t version) override {
    if (interface == generated::ext_session_lock_manager_v1_interface.name) {
      manager_name_ = name;
      manager_version_ = version;
    } else if (interface == generated::wl_output_interface.name) {
      discovered_.emplace_back(name, version);
    }
  }

  void bind() override {
    if (manager_name_ == 0 || discovered_.empty())
      throw std::runtime_error("compositor requires linux-dmabuf, ext_session_lock_manager_v1, and an output");
    manager_ = client::core::bind(display(), registry(), manager_name_,
        generated::ext_session_lock_manager_v1_interface,
        std::min(manager_version_, generated::ext_session_lock_manager_v1_interface.version));
    if (manager_ == nullptr) throw std::runtime_error("could not bind session lock manager");
    for (const auto& [name, version] : std::exchange(discovered_, {})) add_output(name, version);
    if (!display().roundtrip()) throw std::runtime_error("Wayland output roundtrip failed");
    // Outputs come and go while locked; each needs its own lock surface.
    struct RegistryChanges {
      LockRole* role;
      void global(client::Proxy&, std::uint32_t name, const std::string& interface,
                  std::uint32_t version) {
        if (interface == generated::wl_output_interface.name) role->add_output(name, version);
      }
      void global_remove(client::Proxy&, std::uint32_t name) { role->remove_output(name); }
    };
    registry().set_observer(generated::wl_registry_observer(RegistryChanges{this}));
  }

  void create() override {
    reset_session_authentication();
    const auto lock_id = generated::ext_session_lock_manager_v1_lock(display(), manager_->id);
    lock_ = display().find_proxy(lock_id);
    if (lock_ == nullptr) throw std::runtime_error("could not create session lock");
    struct LockEvents {
      LockRole* role;
      void locked(client::Proxy&) {
        role->locked_ = true;
        set_session_locked(true);
      }
      void finished(client::Proxy&) {
        if (role->lock_ != nullptr) {
          if (role->locked_) generated::ext_session_lock_v1_unlock_and_destroy(role->display(), role->lock_->id);
          else generated::ext_session_lock_v1_destroy(role->display(), role->lock_->id);
          role->lock_ = nullptr;
          set_session_locked(false);
          role->flush("could not finish session lock");
        }
        role->close();
      }
    };
    lock_->set_observer(generated::ext_session_lock_v1_observer(LockEvents{this}));
    if (!display().roundtrip()) throw std::runtime_error("session lock request failed");
    if (lock_ == nullptr) return;
    surfaces_started_ = true;
    for (const auto& output : outputs_) create_lock_surface(*output);
    flush("could not flush session lock request");
  }

  // Lock surfaces show their own buffers (see reload); there is no shared one.
  void present(bool) override {}
  bool ready() const override { return true; }

  bool refresh() override {
    for (const auto& output : outputs_)
      if (output->configured) render_output(*output);
    return true;
  }

  bool reload(const Scene& scene, const Frame& frame) override {
    if (&scene != &this->scene()) {
      rebuild(true);
      return true;
    }
    if (LockOutput* active = find(active_)) present_output(*active, this->scene(), frame);
    return true;
  }

  bool prepare() override {
    if (lock_ == nullptr || !locked_ || !session_unlock_requested()) return false;
    generated::ext_session_lock_v1_unlock_and_destroy(display(), lock_->id);
    lock_ = nullptr;
    if (!display().roundtrip()) throw std::runtime_error("could not confirm session unlock");
    set_session_locked(false);
    locked_ = false;
    close();
    return true;
  }

  void keyboard_enter(std::uint32_t surface) override {
    keyboard_surface_ = surface;
    activate(surface);
  }
  void keyboard_leave(std::uint32_t surface) override {
    if (keyboard_surface_ == surface) keyboard_surface_ = 0;
  }
  void pointer_enter(std::uint32_t surface) override { activate(surface); }

  void destroy() noexcept override {
    try {
      // A locked session must not receive ext_session_lock_v1.destroy. If the
      // client exits unexpectedly, the compositor stays locked by protocol.
      if (lock_ != nullptr && !locked_) {
        generated::ext_session_lock_v1_destroy(display(), lock_->id);
        lock_ = nullptr;
      }
      for (const auto& entry : outputs_) {
        if (entry->lock_surface != nullptr)
          generated::ext_session_lock_surface_v1_destroy(display(), entry->lock_surface->id);
        if (entry->surface != nullptr) generated::wl_surface_destroy(display(), entry->surface->id);
        if (entry->buffer != nullptr) generated::wl_buffer_destroy(display(), entry->buffer->id);
        if (entry->output != nullptr && entry->output->version >= 3)
          generated::wl_output_release(display(), entry->output->id);
      }
      if (manager_ != nullptr) generated::ext_session_lock_manager_v1_destroy(display(), manager_->id);
      (void)display().flush();
    } catch (...) {}
  }

 private:
  LockOutput* find(std::uint32_t name) const {
    if (name == 0) return nullptr;
    const auto found = std::ranges::find_if(outputs_,
        [name](const auto& entry) { return entry->global_name == name; });
    return found == outputs_.end() ? nullptr : found->get();
  }

  void add_output(std::uint32_t name, std::uint32_t version) {
    if (find(name) != nullptr) return;
    auto entry = std::make_unique<LockOutput>();
    entry->global_name = name;
    entry->name = "output-" + std::to_string(name);
    entry->output = client::core::bind(display(), registry(), name, generated::wl_output_interface,
                                       std::min(version, generated::wl_output_interface.version));
    if (entry->output == nullptr) throw std::runtime_error("could not bind lock output");
    struct OutputEvents {
      LockOutput* entry;
      void geometry(client::Proxy&, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                    std::int32_t, const std::string&, const std::string&, std::int32_t) {}
      void mode(client::Proxy&, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) {}
      void done(client::Proxy&) {}
      void scale(client::Proxy&, std::int32_t) {}
      void name(client::Proxy&, const std::string& name) { entry->name = name; }
      void description(client::Proxy&, const std::string&) {}
    };
    entry->output->set_observer(generated::wl_output_observer(OutputEvents{entry.get()}));
    LockOutput& added = *entry;
    outputs_.push_back(std::move(entry));
    if (lock_ != nullptr && surfaces_started_) {
      create_lock_surface(added);
      flush("could not flush new lock output");
    }
  }

  void remove_output(std::uint32_t name) {
    const auto found = std::ranges::find_if(outputs_,
        [name](const auto& entry) { return entry->global_name == name; });
    if (found == outputs_.end()) return;
    LockOutput& entry = **found;
    if (entry.lock_surface != nullptr)
      generated::ext_session_lock_surface_v1_destroy(display(), entry.lock_surface->id);
    if (entry.surface != nullptr) generated::wl_surface_destroy(display(), entry.surface->id);
    if (entry.buffer != nullptr) generated::wl_buffer_destroy(display(), entry.buffer->id);
    if (entry.output != nullptr) {
      entry.output->set_observer([](client::Proxy& proxy, std::uint32_t opcode,
                                    wire::MessageParser& parser) {
        parser.discard(proxy.interface->events[opcode].signature);
      });
      if (entry.output->version >= 3) generated::wl_output_release(display(), entry.output->id);
    }
    if (active_ == name) {
      active_ = 0;
      surface() = nullptr;
    }
    outputs_.erase(found);
    if (active_ == 0 && !outputs_.empty() && outputs_.front()->configured)
      activate(outputs_.front()->surface->id);
  }

  void create_lock_surface(LockOutput& entry) {
    const auto surface_id = generated::wl_compositor_create_surface(display(), compositor().id);
    entry.surface = display().find_proxy(surface_id);
    if (entry.surface == nullptr) throw std::runtime_error("could not create lock wl_surface");
    const auto lock_id = generated::ext_session_lock_v1_get_lock_surface(
        display(), lock_->id, entry.surface, entry.output);
    entry.lock_surface = display().find_proxy(lock_id);
    if (entry.lock_surface == nullptr) throw std::runtime_error("could not create lock surface");
    struct LockSurfaceEvents {
      LockRole* role;
      std::uint32_t name;
      void configure(client::Proxy&, std::uint32_t serial, std::uint32_t width, std::uint32_t height) {
        role->on_configure(name, serial, width, height);
      }
    };
    entry.lock_surface->set_observer(generated::ext_session_lock_surface_v1_observer(
        LockSurfaceEvents{this, entry.global_name}));
  }

  // Makes the lock surface `surface` the one that takes input.
  void activate(std::uint32_t surface) {
    for (const auto& entry : outputs_) {
      if (entry->surface == nullptr || entry->surface->id != surface || !entry->configured) continue;
      active_ = entry->global_name;
      this->surface() = entry->surface;
      scene() = entry->scene;
      reset_pointer();
      return;
    }
  }

  void on_configure(std::uint32_t name, std::uint32_t serial, std::uint32_t width,
                    std::uint32_t height) {
    LockOutput* entry = find(name);
    if (entry == nullptr) return;
    if (width == 0 || height == 0 || width > 8192 || height > 8192)
      throw std::runtime_error("invalid lock surface dimensions");
    generated::ext_session_lock_surface_v1_ack_configure(display(), entry->lock_surface->id, serial);
    entry->width = width;
    entry->height = height;
    if (can_render()) render_output(*entry);
  }

  void render_output(LockOutput& entry) {
    if (entry.width == 0 || !can_render()) return;
    set_lock_surface_context(entry.width, entry.height, entry.name);
    set_surface_size({entry.width, entry.height});
    const Scene next = build();
    // A locked session cannot be dropped; keep the last lock scene.
    if (!next.mapped) return;
    if (!next.session_lock || next.width != entry.width || next.height != entry.height)
      throw std::runtime_error("lock screen must render at its configured output size");
    if (entry.configured && next.rectangles == entry.scene.rectangles &&
        next.rasters == entry.scene.rasters) {
      entry.scene = next;
      if (active_ == entry.global_name) scene() = next;
      return;
    }
    present_output(entry, next, render(next));
  }

  void present_output(LockOutput& entry, const Scene& scene, const Frame& frame) {
    if (frame.width != entry.width || frame.height != entry.height)
      throw std::runtime_error("lock buffer does not match configured output dimensions");
    create_buffer_for(frame, entry.buffer);
    commit_buffer(*entry.surface, entry.buffer, entry.width, entry.height);
    flush("could not flush lock surface");
    entry.scene = scene;
    entry.configured = true;
    if (active_ == 0 || active_ == entry.global_name || keyboard_surface_ == entry.surface->id)
      activate(entry.surface->id);
  }

  std::uint32_t manager_name_ = 0;
  std::uint32_t manager_version_ = 0;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> discovered_;
  client::Proxy* manager_ = nullptr;
  client::Proxy* lock_ = nullptr;
  bool locked_ = false;
  bool surfaces_started_ = false;
  std::vector<std::unique_ptr<LockOutput>> outputs_;
  std::uint32_t active_ = 0;
  std::uint32_t keyboard_surface_ = 0;
};

}  // namespace

std::unique_ptr<Role> make_lock_role(WaylandWidget& widget) {
  return std::make_unique<LockRole>(widget);
}

}  // namespace vw::roles
