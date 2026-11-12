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

// Readable when compositor state changes, or -1 without zwwm; see
// vw::App::on_event.
int event_fd();
bool drain_events();

}  // namespace vw::zwwm
