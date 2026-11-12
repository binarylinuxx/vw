// Layer-shell surfaces: bars, docks, overlays. Prefers zwwm's native
// layer shell and falls back to wlr-layer-shell; both number their requests
// and enums the same way.
#include "role.hpp"

#include <wlr-layer-shell-unstable-v1-zwayland-client.h>
#include <zwwm-layer-shell-v1-zwayland-client.h>
#include <wayland-zwayland-client.h>
#include <zwayland/client/core.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace vw::roles {
namespace {

using namespace zwayland;

struct Reservation {
  std::uint32_t anchor;
  std::int32_t zone;
};

Reservation reservation(const Scene& scene, std::uint32_t height) {
  std::uint32_t anchor = scene.anchor;
  std::int32_t zone = scene.exclusive_zone;
  if (scene.exclusive && zone == 0) zone = static_cast<std::int32_t>(height);
  if (zone > 0) {
    constexpr std::uint32_t top = 1;
    constexpr std::uint32_t bottom = 2;
    constexpr std::uint32_t left = 4;
    constexpr std::uint32_t right = 8;
    if ((anchor & (top | bottom)) == top || (anchor & (top | bottom)) == bottom)
      anchor |= left | right;
    else if ((anchor & (left | right)) == left || (anchor & (left | right)) == right)
      anchor |= top | bottom;
  }
  return {anchor, zone};
}

std::uint32_t keyboard_interactivity(const Scene& scene) {
  switch (scene.keyboard) {
    case Keyboard::none: return generated::ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
    case Keyboard::exclusive: return generated::ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
    case Keyboard::on_demand: return generated::ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND;
    case Keyboard::automatic: break;
  }
  return !scene.text_fields.empty() || scene.on_key
      ? generated::ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND
      : generated::ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
}

// Everything set on the layer surface besides its size, as last sent.
struct Placement {
  Reservation reserved{};
  std::uint32_t layer = 0;
  std::array<std::int32_t, 4> margin{};
  std::uint32_t keyboard = 0;

  bool operator==(const Placement& other) const {
    return reserved.anchor == other.reserved.anchor && reserved.zone == other.reserved.zone &&
           layer == other.layer && margin == other.margin && keyboard == other.keyboard;
  }
};

class LayerRole final : public Role {
 public:
  using Role::Role;

  void global(std::uint32_t name, std::string_view interface, std::uint32_t version) override {
    if (interface == generated::zwwm_layer_shell_v1_interface.name) {
      native_name_ = name;
      native_version_ = version;
    } else if (interface == generated::zwlr_layer_shell_v1_interface.name) {
      wlr_name_ = name;
      wlr_version_ = version;
    }
  }

  void bind() override {
    native_ = native_name_ != 0;
    if (native_) {
      shell_ = client::core::bind(display(), registry(), native_name_,
          generated::zwwm_layer_shell_v1_interface,
          std::min(native_version_, generated::zwwm_layer_shell_v1_interface.version));
    } else if (wlr_name_ != 0) {
      shell_ = client::core::bind(display(), registry(), wlr_name_,
          generated::zwlr_layer_shell_v1_interface,
          std::min(wlr_version_, generated::zwlr_layer_shell_v1_interface.version));
    } else {
      throw std::runtime_error("compositor requires wl_compositor, linux-dmabuf, and a layer-shell protocol");
    }
    if (shell_ == nullptr) throw std::runtime_error("could not bind required Wayland globals");
  }

  bool focus_grab() const override { return true; }

  void create() override {
    create_surface();
    const Scene& scene = this->scene();
    const std::uint32_t id = native_
        ? generated::zwwm_layer_shell_v1_get_layer_surface(
              display(), shell_->id, surface(), nullptr, scene.layer, scene.surface_namespace)
        : generated::zwlr_layer_shell_v1_get_layer_surface(
              display(), shell_->id, surface(), nullptr, scene.layer, scene.surface_namespace);
    layer_surface_ = display().find_proxy(id);
    if (layer_surface_ == nullptr) throw std::runtime_error("could not create layer surface");
    start_focus_grab();
    struct Events {
      LayerRole* role;
      void configure(client::Proxy&, std::uint32_t serial, std::uint32_t width,
                     std::uint32_t height) {
        role->on_configure(serial, width, height);
      }
      void closed(client::Proxy&) { role->close(); }
    };
    if (native_) layer_surface_->set_observer(generated::zwwm_layer_surface_v1_observer(Events{this}));
    else layer_surface_->set_observer(generated::zwlr_layer_surface_v1_observer(Events{this}));
    send_size();
    send_placement(placement());
    generated::wl_surface_commit(display(), surface()->id);
    flush("could not flush initial Wayland surface state");
  }

  void present(bool size_changed) override {
    if (size_changed) send_size();
    set_input_region();
    if (const Placement next = placement(); !(next == sent_)) send_placement(next);
    commit_buffer(*surface(), buffer(), width(), height());
    update_focus_grab();
    flush("could not flush reloaded widget");
  }

 private:
  template <auto Native, auto Wlr, typename... Arguments>
  void send(Arguments... arguments) {
    if (native_) Native(display(), layer_surface_->id, arguments...);
    else Wlr(display(), layer_surface_->id, arguments...);
  }

  Placement placement() const {
    const Scene& scene = this->scene();
    return {reservation(scene, height()), scene.layer, scene.margin, keyboard_interactivity(scene)};
  }

  void send_size() {
    const Scene& scene = this->scene();
    send<&generated::zwwm_layer_surface_v1_set_size, &generated::zwlr_layer_surface_v1_set_size>(
        scene.fill_width ? 0U : width(), scene.fill_height ? 0U : height());
  }

  void send_placement(const Placement& next) {
    send<&generated::zwwm_layer_surface_v1_set_anchor, &generated::zwlr_layer_surface_v1_set_anchor>(
        next.reserved.anchor);
    // The layer was given at creation; only a later change needs sending.
    if (layer_surface_created_)
      send<&generated::zwwm_layer_surface_v1_set_layer, &generated::zwlr_layer_surface_v1_set_layer>(
          next.layer);
    send<&generated::zwwm_layer_surface_v1_set_margin, &generated::zwlr_layer_surface_v1_set_margin>(
        next.margin[0], next.margin[1], next.margin[2], next.margin[3]);
    send<&generated::zwwm_layer_surface_v1_set_exclusive_zone,
         &generated::zwlr_layer_surface_v1_set_exclusive_zone>(next.reserved.zone);
    send<&generated::zwwm_layer_surface_v1_set_keyboard_interactivity,
         &generated::zwlr_layer_surface_v1_set_keyboard_interactivity>(next.keyboard);
    sent_ = next;
    layer_surface_created_ = true;
  }

  void on_configure(std::uint32_t serial, std::uint32_t width, std::uint32_t height) {
    set_configured_size(width == 0 ? this->width() : width, height == 0 ? this->height() : height);
    send<&generated::zwwm_layer_surface_v1_ack_configure,
         &generated::zwlr_layer_surface_v1_ack_configure>(serial);
    // A fill axis takes whatever size the compositor picked; rebuild for it.
    const auto resized = [this] {
      const Scene& scene = this->scene();
      return (scene.fill_width && configured_width() != this->width()) ||
             (scene.fill_height && configured_height() != this->height());
    };
    if (configured()) {
      if (resized()) rebuild();
      flush("could not acknowledge layer configure");
      return;
    }
    if (resized() && can_render()) {
      // The first frame goes out at the configured size; present() attaches it.
      mark_configured();
      rebuild();
      if (!resized()) {
        update_focus_grab();
        flush("could not present configured layer surface");
        return;
      }
      // The rebuild failed; present the content-sized frame rather than nothing.
    }
    commit_buffer(*surface(), buffer(), this->width(), this->height());
    mark_configured();
    update_focus_grab();
    flush("could not present configured layer surface");
  }

  std::uint32_t native_name_ = 0;
  std::uint32_t native_version_ = 0;
  std::uint32_t wlr_name_ = 0;
  std::uint32_t wlr_version_ = 0;
  bool native_ = false;
  client::Proxy* shell_ = nullptr;
  client::Proxy* layer_surface_ = nullptr;
  Placement sent_;
  bool layer_surface_created_ = false;
};

}  // namespace

std::unique_ptr<Role> make_layer_role(WaylandWidget& widget) {
  return std::make_unique<LayerRole>(widget);
}

}  // namespace vw::roles
