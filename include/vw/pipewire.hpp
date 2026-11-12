#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// PipeWire audio devices and streams. The connection starts on first use.
namespace vw::pipewire {

struct Node {
  std::uint32_t id = 0;
  std::string name;
  std::string description;
  // "Audio/Sink", "Audio/Source", "Stream/Output/Audio", ...
  std::string media_class;
  std::string app_name;
  std::string state = "creating";
  std::vector<float> channel_volumes;
  bool muted = false;
  bool volume_known = false;
  bool can_control = false;

  // Mean of the channels, as PipeWire stores it.
  double linear_volume() const {
    if (channel_volumes.empty()) return 0;
    double value = 0;
    for (float channel : channel_volumes) value += channel;
    return value / static_cast<double>(channel_volumes.size());
  }
  // Perceptual (cubic) volume, the scale used by mixers and set_volume().
  double volume() const { return std::cbrt(linear_volume()); }

  bool operator==(const Node&) const = default;
};

enum class Filter { all, sinks, sources, streams };

struct Result {
  bool ok = false;
  std::string error;
};

bool available();
// The connection error, if any.
std::string error();
std::vector<Node> nodes(Filter filter = Filter::all);
std::optional<Node> default_sink();
std::optional<Node> default_source();

// volume is perceptual, 0 to 4.
Result set_volume(std::uint32_t id, double volume);
Result set_mute(std::uint32_t id, bool muted);
Result toggle_mute(std::uint32_t id);
Result set_default_sink(std::uint32_t id);
Result set_default_source(std::uint32_t id);

// Readable when devices or volumes change; see vw::App::on_event.
int event_fd();
bool drain_events();

}  // namespace vw::pipewire
