#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "vw/renderer.hpp"
#include "vw/scene.hpp"

namespace zwayland::client {
class Display;
struct Proxy;
}

namespace vw {

class SceneSource;
namespace roles { class Role; }

// Drives every surface on one event loop until a surface without a close
// handler closes, SIGINT/SIGTERM arrives, or a toplevel requests close.
// `shared` supplies event sources and file watching; each entry of
// `surfaces` builds one surface, which is created when its scene maps and
// destroyed when it unmaps.
void run_surfaces(SceneSource& shared, std::span<SceneSource* const> surfaces, Renderer& renderer);

// One surface and its own Wayland connection.
class WaylandWidget {
 public:
  WaylandWidget(const Scene& scene, const Frame& frame);
  ~WaylandWidget();
  WaylandWidget(const WaylandWidget&) = delete;
  WaylandWidget& operator=(const WaylandWidget&) = delete;

  // Event-loop steps, called by run_surfaces.
  void attach(SceneSource& source, Renderer& renderer);
  int fd() const;
  // Before poll: finish an authenticated lock and request animation frames.
  void prepare();
  // After poll; false when the connection failed or hung up.
  bool dispatch(short revents);
  // Rebuilds from the source; input=true marks it as caused by user input,
  // which makes the other surfaces rebuild too.
  void refresh(bool input) { refresh_scene(input); }
  // Handles a delivered animation frame callback.
  void tick(std::chrono::steady_clock::time_point now);
  // When the next key repeat is due, and firing it.
  std::optional<std::chrono::steady_clock::time_point> deadline() const;
  void timers(std::chrono::steady_clock::time_point now);
  bool take_focus_events() { return dispatch_focus_events(); }
  bool closed_by_compositor() const { return closed_; }
  // The scene unmapped or changed role; the driver drops or recreates it.
  bool retired() const { return retired_; }
  bool animating() const;
  std::uint64_t input_epoch() const { return seen_epoch_; }
  void mark_epoch(std::uint64_t epoch) { seen_epoch_ = epoch; }
  const Scene& scene() const { return scene_; }
  void throw_if_protocol_error() const;

  // Clipboard on this surface's connection (see vw::set_clipboard).
  void copy_to_clipboard(std::string text);
  std::optional<std::string> paste_from_clipboard();

  void pointer_motion(double x, double y);
  void pointer_leave();
  void pointer_button(std::uint32_t button, std::uint32_t state);
  void pointer_axis(double vertical);

 private:
  // Roles (src/roles) present the surface; Role's helpers reach in here.
  friend class roles::Role;

  void discover_globals();
  void create_pointer();
  void create_keyboard();
  void refresh_scene(bool input = true);
  void keyboard_key(std::uint32_t key, bool repeat);
  void stop_key_repeat() { repeat_key_ = 0; }
  void create_data_device();
  void create_control_device();
  void forget_offer(zwayland::client::Proxy*& offer);
  void forget_control_offer(zwayland::client::Proxy*& offer);
  // Text field under the pointer: focuses it and returns the caret offset
  // nearest x.
  std::size_t caret_at(const TextField& field, const Interaction& interaction, double x) const;
  void text_pointer_press(std::size_t interaction_index);
  void create_buffer(const Frame& frame);
  void create_buffer_for(const Frame& frame, zwayland::client::Proxy*& target);
  void configure_surface_input(const Scene& scene);
  void reload(const Scene& scene, const Frame& frame);
  bool animate_hover(float elapsed_seconds);
  void request_animation_frame();
  // Whether another frame is wanted (a tween, a hover fade, a pending drag).
  bool wants_frame() const;
  // Asks for wl_surface.frame on the next commit of `surface`, so the
  // callback rides on the buffer commit instead of an extra empty one.
  void add_frame_callback(std::uint32_t surface);
  void dispatch_drag(const DragHandler& handler, bool cancelled = false);
  void end_drag(bool cancelled);
  void create_focus_grab();
  void update_focus_grab();
  bool dispatch_focus_events();
  std::unique_ptr<zwayland::client::Display> display_;
  zwayland::client::Proxy* compositor_ = nullptr;
  zwayland::client::Proxy* dmabuf_ = nullptr;
  zwayland::client::Proxy* focus_grab_manager_ = nullptr;
  zwayland::client::Proxy* focus_grab_ = nullptr;
  bool focus_grab_requested_ = false;
  struct PendingFocus { bool activated; std::uint32_t reason; };
  std::vector<PendingFocus> focus_events_;
  zwayland::client::Proxy* registry_ = nullptr;
  zwayland::client::Proxy* surface_ = nullptr;
  zwayland::client::Proxy* buffer_ = nullptr;
  zwayland::client::Proxy* seat_ = nullptr;
  zwayland::client::Proxy* pointer_ = nullptr;
  zwayland::client::Proxy* keyboard_ = nullptr;
  struct KeyboardState;
  std::unique_ptr<KeyboardState> keyboard_state_;
  Scene scene_;
  std::unique_ptr<roles::Role> role_;
  Renderer* renderer_ = nullptr;
  int hovered_ = -1;
  int pressed_ = -1;
  double pointer_x_ = 0;
  double pointer_y_ = 0;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::uint32_t configured_width_ = 0;
  std::uint32_t configured_height_ = 0;
  bool configured_ = false;
  bool closed_ = false;
  bool pointer_inside_ = false;
  std::uint32_t frame_callback_ = 0;
  bool animation_frame_ready_ = false;
  std::chrono::steady_clock::time_point animation_last_tick_{};
  bool dragging_ = false;
  DragHandler drag_move_;
  DragHandler drag_end_;
  std::uint32_t drag_button_ = 0;
  double drag_start_x_ = 0;
  double drag_start_y_ = 0;
  double drag_previous_x_ = 0;
  double drag_previous_y_ = 0;
  bool drag_refresh_pending_ = false;
  std::chrono::steady_clock::time_point drag_frame_at_{};
  SceneSource* source_ = nullptr;
  bool retired_ = false;
  std::uint64_t seen_epoch_ = 0;
  // Key repeat; repeat_key_ is 0 while no key repeats. Rate 0 disables it.
  std::uint32_t repeat_key_ = 0;
  std::int32_t repeat_rate_ = 25;
  std::int32_t repeat_delay_ = 600;
  std::chrono::steady_clock::time_point repeat_at_{};
  // Serial of the latest key or button press; set_selection requires one.
  std::uint32_t input_serial_ = 0;
  zwayland::client::Proxy* data_manager_ = nullptr;
  zwayland::client::Proxy* data_device_ = nullptr;
  zwayland::client::Proxy* selection_offer_ = nullptr;
  zwayland::client::Proxy* drag_offer_ = nullptr;
  // wlr-data-control: clipboard access without keyboard focus or a serial,
  // used in preference to wl_data_device when the compositor offers it.
  zwayland::client::Proxy* control_manager_ = nullptr;
  zwayland::client::Proxy* control_device_ = nullptr;
  zwayland::client::Proxy* control_offer_ = nullptr;
  zwayland::client::Proxy* control_primary_offer_ = nullptr;
  std::unordered_map<std::uint32_t, std::vector<std::string>> offer_types_;
  // Cancelled data sources, destroyed after dispatch: a proxy must not be
  // destroyed from inside its own event handler.
  // (id, data control?)
  std::vector<std::pair<std::uint32_t, bool>> dead_sources_;
  // Mouse selection inside a text field, by field id since rebuilds replace
  // the scene; and click counting for word/all selection.
  std::string text_drag_id_;
  std::string last_click_id_;
  std::chrono::steady_clock::time_point last_click_{};
  int click_count_ = 0;
};

}  // namespace vw
