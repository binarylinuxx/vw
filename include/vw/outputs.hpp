#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The monitors, as the compositor lays them out. Sizes are logical: what a
// full-width layer surface on that output measures, in the units vw draws
// in. vw::App watches them while it runs, so a build that reads them is
// rebuilt when a monitor is plugged in, removed or changes mode.
//
//   for (const vw::Output& output : vw::outputs()) …
//   app.per_output([](vw::Ui& ui, const vw::Output& output) { /* a bar */ });
namespace vw {

struct Output {
  std::string name;         // the connector: "DP-1", "eDP-1", "HDMI-A-1"
  std::string description;  // usually make and model
  std::int32_t x = 0;       // logical position in the output layout
  std::int32_t y = 0;
  std::uint32_t width = 0;  // logical size
  std::uint32_t height = 0;
  std::uint32_t pixel_width = 0;  // the current mode
  std::uint32_t pixel_height = 0;
  double scale = 1.0;          // pixels per logical unit
  std::uint32_t refresh = 0;   // mHz: 60000 is 60 Hz
};

// Every output with a name and a size, in the compositor's order. Empty
// without a Wayland connection.
std::vector<Output> outputs();
// The output called `name`, if it is connected.
std::optional<Output> output(std::string_view name);

// Readable when outputs changed; see vw::App::on_event. vw::App already
// watches it. -1 without a Wayland connection.
int output_event_fd();
bool drain_output_events();

}  // namespace vw
