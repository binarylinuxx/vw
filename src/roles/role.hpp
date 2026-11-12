#pragma once

// Surface roles: how a WaylandWidget's content becomes something the
// compositor shows, such as a layer-shell panel, an xdg toplevel or
// session-lock surfaces.
//
// WaylandWidget owns the connection, the buffer, input, the clipboard and
// frame pacing. A role owns its protocol objects, acknowledges configures,
// and decides when and how a buffer is shown. To add one, subclass Role in a
// file of its own, give it a Kind and pick it in make_role (role.cpp).

#include "vw/renderer.hpp"
#include "vw/scene.hpp"
#include "vw/wayland.hpp"

#include <zwayland/client/display.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

namespace vw::roles {

namespace client = zwayland::client;

enum class Kind { layer, toplevel, lock };

// The role a scene asks for. A rebuild that changes it retires the surface.
Kind kind(const Scene& scene);
// Whether the widget renders one buffer for the role. The lock role renders
// its own buffer for each output.
inline bool uses_widget_buffer(const Scene& scene) { return kind(scene) != Kind::lock; }

class Role {
 public:
  explicit Role(WaylandWidget& widget) : widget_(widget) {}
  virtual ~Role() = default;
  Role(const Role&) = delete;
  Role& operator=(const Role&) = delete;

  // Registry discovery, called for each global vw's core does not use itself.
  virtual void global(std::uint32_t name, std::string_view interface, std::uint32_t version) = 0;
  // Binds what global() found. Throws when the compositor lacks it.
  virtual void bind() = 0;
  // Creates the role objects and commits the initial state.
  virtual void create() = 0;
  // Shows the widget's buffer for scene(). Called once ready().
  virtual void present(bool size_changed) = 0;
  // Whether frames may be presented (the initial configure was acked).
  virtual bool ready() const { return configured(); }
  // Rebuild and reload hooks for roles that render on their own; true when
  // the role handled it.
  virtual bool refresh() { return false; }
  virtual bool reload(const Scene&, const Frame&) { return false; }
  // Before each poll; true when the role used up the wakeup.
  virtual bool prepare() { return false; }
  virtual void keyboard_enter(std::uint32_t /*surface*/) {}
  virtual void keyboard_leave(std::uint32_t /*surface*/) {}
  virtual void pointer_enter(std::uint32_t /*surface*/) {}
  // Whether the zwwm focus grab applies to this role.
  virtual bool focus_grab() const { return false; }
  // Destroys the role objects while the connection is still up. Must not throw.
  virtual void destroy() noexcept {}

 protected:
  // What a role may use of its widget. Implemented in wayland.cpp.
  client::Display& display() const;
  client::Proxy& registry() const;
  client::Proxy& compositor() const;
  // The surface that receives input and frame callbacks.
  client::Proxy*& surface() const;
  Scene& scene() const;
  client::Proxy* buffer() const;
  std::uint32_t width() const;
  std::uint32_t height() const;
  void set_configured_size(std::uint32_t width, std::uint32_t height) const;
  std::uint32_t configured_width() const;
  std::uint32_t configured_height() const;
  bool configured() const;
  void mark_configured() const;
  bool can_render() const;
  // Builds the scene at the current surface size, and renders one.
  Scene build() const;
  Frame render(const Scene& scene) const;
  void rebuild(bool input = false) const;
  // Creates surface() with the scene's input region.
  void create_surface() const;
  void set_input_region() const;
  void create_buffer_for(const Frame& frame, client::Proxy*& target) const;
  // Attaches and damages a buffer, asks for a frame callback when an
  // animation wants one, and commits.
  void commit_buffer(client::Proxy& surface, client::Proxy* buffer,
                     std::uint32_t width, std::uint32_t height) const;
  void close() const;
  // Forgets hover and press state, for when surface() changes.
  void reset_pointer() const;
  void start_focus_grab() const;
  void update_focus_grab() const;
  void flush(const char* what) const;

 private:
  WaylandWidget& widget_;
};

std::unique_ptr<Role> make_layer_role(WaylandWidget& widget);
std::unique_ptr<Role> make_toplevel_role(WaylandWidget& widget);
std::unique_ptr<Role> make_lock_role(WaylandWidget& widget);

std::unique_ptr<Role> make_role(const Scene& scene, WaylandWidget& widget);

}  // namespace vw::roles
