#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The zwwm compositor's manager and tags protocols, on a separate Wayland
// connection. Calls throw when the protocol they need is unavailable.
namespace vw::zwwm {

enum class Snapshot : std::uint32_t {
  status, outputs, toplevels, output_tags, layers, cameras, keyboard, live
};

struct Tag {
  std::uint32_t id = 0;
  std::string name;
  bool active = false;
};

struct Result {
  bool success = false;
  std::string message;
};

bool available();
std::vector<Tag> tags();
std::vector<std::uint32_t> active_tags();
void set_active_tags(const std::vector<std::uint32_t>& ids);
// Most recent manager subscription event.
std::string last_event();

nlohmann::json snapshot(Snapshot type);
Result dispatch(std::string_view action, std::string_view argument = {});
Result reload();
Result rebuild_switch_shaders();
Result set_cursor(std::string_view theme, std::uint32_t size);

// Touch swipes that start at an output edge. The compositor recognises them
// and keeps the touch away from other clients; the shell decides what a
// swipe does, e.g. drags a sidebar open with `distance`.
enum Edge : std::uint32_t { top = 1, bottom = 2, left = 4, right = 8 };

struct EdgeSwipe {
  enum class Phase { began, progress, ended };
  Phase phase = Phase::began;
  Edge edge = top;
  // Where the touch went down, output-local; set on began.
  float x = 0;
  float y = 0;
  // Logical pixels from the touch-down point: along the swipe (positive
  // towards the inside of the output) and across it.
  float distance = 0;
  float lateral = 0;
  // Along the swipe, in logical pixels per second; set on ended.
  float velocity = 0;
  bool cancelled = false;
};

// Follows swipes from `edges` (Edge bits ORed together); 0 stops. Swipes are
// delivered through event_fd()/drain_events() and collected with
// take_edge_swipes(), oldest first. Throws without the protocol.
void watch_edge_swipes(std::uint32_t edges);
std::vector<EdgeSwipe> take_edge_swipes();

// Readable when compositor state changes, or -1 without zwwm; see
// vw::App::on_event.
int event_fd();
bool drain_events();

}  // namespace vw::zwwm
