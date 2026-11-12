// xdg-shell toplevels: ordinary application windows.
#include "role.hpp"

#include "vw/host.hpp"

#include <xdg-shell-zwayland-client.h>
#include <wayland-zwayland-client.h>
#include <zwayland/client/core.hpp>

#include <algorithm>
#include <span>
#include <stdexcept>

namespace vw::roles {
namespace {

using namespace zwayland;

class ToplevelRole final : public Role {
 public:
  using Role::Role;

  void global(std::uint32_t name, std::string_view interface, std::uint32_t version) override {
    if (interface != generated::xdg_wm_base_interface.name) return;
    wm_name_ = name;
    wm_version_ = version;
  }

  void bind() override {
    if (wm_name_ == 0)
      throw std::runtime_error("compositor requires wl_compositor, linux-dmabuf, and xdg_wm_base");
    wm_base_ = client::core::bind(display(), registry(), wm_name_, generated::xdg_wm_base_interface,
        std::min(wm_version_, generated::xdg_wm_base_interface.version));
    if (wm_base_ == nullptr) throw std::runtime_error("could not bind xdg_wm_base");
    struct WmEvents {
      client::Display* display;
      void ping(client::Proxy& proxy, std::uint32_t serial) {
        generated::xdg_wm_base_pong(*display, proxy.id, serial);
        if (!display->flush()) throw std::runtime_error("could not answer xdg_wm_base ping");
      }
    };
    wm_base_->set_observer(generated::xdg_wm_base_observer(WmEvents{&display()}));
  }

  void create() override {
    create_surface();
    const auto xdg_id = generated::xdg_wm_base_get_xdg_surface(display(), wm_base_->id, surface());
    xdg_surface_ = display().find_proxy(xdg_id);
    if (xdg_surface_ == nullptr) throw std::runtime_error("could not create xdg_surface");
    struct SurfaceEvents {
      ToplevelRole* role;
      void configure(client::Proxy&, std::uint32_t serial) { role->on_configure(serial); }
    };
    xdg_surface_->set_observer(generated::xdg_surface_observer(SurfaceEvents{this}));
    const auto toplevel_id = generated::xdg_surface_get_toplevel(display(), xdg_surface_->id);
    toplevel_ = display().find_proxy(toplevel_id);
    if (toplevel_ == nullptr) throw std::runtime_error("could not create xdg_toplevel");
    struct ToplevelEvents {
      ToplevelRole* role;
      void configure(client::Proxy&, std::int32_t width, std::int32_t height,
                     std::span<const std::byte>) {
        role->pending_width_ = width > 0 ? static_cast<std::uint32_t>(width) : 0;
        role->pending_height_ = height > 0 ? static_cast<std::uint32_t>(height) : 0;
      }
      void close(client::Proxy&) { role->close(); }
      void configure_bounds(client::Proxy&, std::int32_t, std::int32_t) {}
      void wm_capabilities(client::Proxy&, std::span<const std::byte>) {}
    };
    toplevel_->set_observer(generated::xdg_toplevel_observer(ToplevelEvents{this}));
    generated::xdg_toplevel_set_title(display(), toplevel_->id, scene().title);
    generated::xdg_toplevel_set_app_id(display(), toplevel_->id, scene().app_id);
    generated::wl_surface_commit(display(), surface()->id);
    flush("could not flush initial xdg surface state");
  }

  void present(bool) override {
    set_input_region();
    generated::xdg_toplevel_set_title(display(), toplevel_->id, scene().title);
    generated::xdg_toplevel_set_app_id(display(), toplevel_->id, scene().app_id);
    generated::xdg_surface_set_window_geometry(display(), xdg_surface_->id, 0, 0, width(), height());
    commit_buffer(*surface(), buffer(), width(), height());
    flush("could not flush reloaded xdg surface");
  }

  void destroy() noexcept override {
    if (toplevel_ == nullptr) return;
    try {
      generated::xdg_toplevel_destroy(display(), toplevel_->id);
      generated::xdg_surface_destroy(display(), xdg_surface_->id);
      generated::xdg_wm_base_destroy(display(), wm_base_->id);
      (void)display().flush();
    } catch (...) {}
  }

 private:
  void on_configure(std::uint32_t serial) {
    generated::xdg_surface_ack_configure(display(), xdg_surface_->id, serial);
    const std::uint32_t target_width = pending_width_ == 0 ? width() : pending_width_;
    const std::uint32_t target_height = pending_height_ == 0 ? height() : pending_height_;
    set_toplevel_size(pending_width_, pending_height_);
    pending_width_ = 0;
    pending_height_ = 0;
    set_configured_size(target_width, target_height);
    const bool resized = target_width != width() || target_height != height();
    if (!configured()) {
      generated::xdg_surface_set_window_geometry(display(), xdg_surface_->id, 0, 0, width(), height());
      generated::wl_surface_attach(display(), surface()->id, buffer(), 0, 0);
      generated::wl_surface_damage_buffer(display(), surface()->id, 0, 0,
                                          static_cast<std::int32_t>(width()),
                                          static_cast<std::int32_t>(height()));
      mark_configured();
    }
    generated::wl_surface_commit(display(), surface()->id);
    flush("could not flush configured xdg surface");
    if (resized && can_render()) rebuild();
  }

  std::uint32_t wm_name_ = 0;
  std::uint32_t wm_version_ = 0;
  client::Proxy* wm_base_ = nullptr;
  client::Proxy* xdg_surface_ = nullptr;
  client::Proxy* toplevel_ = nullptr;
  std::uint32_t pending_width_ = 0;
  std::uint32_t pending_height_ = 0;
};

}  // namespace

std::unique_ptr<Role> make_toplevel_role(WaylandWidget& widget) {
  return std::make_unique<ToplevelRole>(widget);
}

}  // namespace vw::roles
