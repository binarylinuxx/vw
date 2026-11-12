#include "role.hpp"
#include "vw/app.hpp"

#include <wayland-zwayland-client.h>

#include <stdexcept>

namespace vw::roles {

using namespace zwayland;

Kind kind(const Scene& scene) {
  if (scene.session_lock) return Kind::lock;
  if (scene.toplevel) return Kind::toplevel;
  return Kind::layer;
}

std::unique_ptr<Role> make_role(const Scene& scene, WaylandWidget& widget) {
  switch (kind(scene)) {
    case Kind::lock: return make_lock_role(widget);
    case Kind::toplevel: return make_toplevel_role(widget);
    case Kind::layer: break;
  }
  return make_layer_role(widget);
}

client::Display& Role::display() const { return *widget_.display_; }
client::Proxy& Role::registry() const { return *widget_.registry_; }
client::Proxy& Role::compositor() const { return *widget_.compositor_; }
client::Proxy*& Role::surface() const { return widget_.surface_; }
Scene& Role::scene() const { return widget_.scene_; }
client::Proxy* Role::buffer() const { return widget_.buffer_; }
std::uint32_t Role::width() const { return widget_.width_; }
std::uint32_t Role::height() const { return widget_.height_; }

void Role::set_configured_size(std::uint32_t width, std::uint32_t height) const {
  widget_.configured_width_ = width;
  widget_.configured_height_ = height;
}

std::uint32_t Role::configured_width() const { return widget_.configured_width_; }
std::uint32_t Role::configured_height() const { return widget_.configured_height_; }
bool Role::configured() const { return widget_.configured_; }
void Role::mark_configured() const { widget_.configured_ = true; }
bool Role::can_render() const { return widget_.renderer_ != nullptr && widget_.source_ != nullptr; }
Scene Role::build() const { return widget_.source_->build(); }
Frame Role::render(const Scene& scene) const { return widget_.renderer_->render(scene); }
void Role::rebuild(bool input) const { widget_.refresh_scene(input); }

void Role::create_surface() const {
  const std::uint32_t id = generated::wl_compositor_create_surface(display(), compositor().id);
  widget_.surface_ = display().find_proxy(id);
  if (widget_.surface_ == nullptr) throw std::runtime_error("could not create wl_surface");
  set_input_region();
}

void Role::set_input_region() const { widget_.configure_surface_input(widget_.scene_); }

void Role::create_buffer_for(const Frame& frame, client::Proxy*& target) const {
  widget_.create_buffer_for(frame, target);
}

void Role::commit_buffer(client::Proxy& surface, client::Proxy* buffer,
                         std::uint32_t width, std::uint32_t height) const {
  generated::wl_surface_attach(display(), surface.id, buffer, 0, 0);
  generated::wl_surface_damage_buffer(display(), surface.id, 0, 0,
                                      static_cast<std::int32_t>(width),
                                      static_cast<std::int32_t>(height));
  if (widget_.wants_frame()) widget_.add_frame_callback(surface.id);
  generated::wl_surface_commit(display(), surface.id);
}

void Role::close() const { widget_.closed_ = true; }

void Role::reset_pointer() const {
  widget_.hovered_ = -1;
  widget_.pressed_ = -1;
}

void Role::start_focus_grab() const { widget_.create_focus_grab(); }
void Role::update_focus_grab() const { widget_.update_focus_grab(); }

void Role::flush(const char* what) const {
  if (!display().flush())
    throw std::runtime_error(display().has_error() ? display().error_message() : what);
}

}  // namespace vw::roles
