#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Batteries and power supplies from UPower, plus power-profiles-daemon, on
// the system bus. State is kept fresh by a background thread; pass
// event_fd()/drain_events() to vw::App::on_event to rebuild on changes.
//
//   if (auto battery = vw::upower::display_device())
//     label = std::to_string(int(battery->percentage)) + "%";
namespace vw::upower {

// UPower's UpDeviceKind.
enum class Kind : std::uint32_t {
  unknown, line_power, battery, ups, monitor, mouse, keyboard, pda, phone, media_player,
  tablet, computer, gaming_input, pen, touchpad, modem, network, headset, speakers,
  headphones, video, other_audio, remote_control, printer, scanner, camera, wearable, toy,
  bluetooth_generic,
};

// UPower's UpDeviceState.
enum class State : std::uint32_t {
  unknown, charging, discharging, empty, fully_charged, pending_charge, pending_discharge,
};

struct Device {
  // D-Bus object path; stable while the device exists.
  std::string path;
  std::string native_path;
  std::string vendor;
  std::string model;
  Kind kind = Kind::unknown;
  State state = State::unknown;
  // 0 to 100.
  double percentage = 0;
  // Wh and W.
  double energy = 0;
  double energy_full = 0;
  double energy_rate = 0;
  // Seconds; 0 when unknown.
  std::int64_t time_to_empty = 0;
  std::int64_t time_to_full = 0;
  bool present = false;
  bool power_supply = false;
  bool rechargeable = false;
  // Freedesktop icon name, e.g. "battery-good-charging-symbolic".
  std::string icon_name;
  // 1 none, 2 discharging, 3 low, 4 critical, 5 action.
  std::uint32_t warning_level = 1;

  bool charging() const { return state == State::charging || state == State::fully_charged; }
};

bool available();
bool on_battery();
// UPower's composite "display device": the combined laptop battery. nullopt
// on machines without one (desktops) or when UPower is missing.
std::optional<Device> display_device();
// All devices, including peripherals such as mice and headsets.
std::vector<Device> devices();

// power-profiles-daemon. Profiles are usually "power-saver", "balanced" and
// "performance"; empty when the daemon is not running.
std::vector<std::string> profiles();
std::string profile();
bool set_profile(std::string_view name);

int event_fd();
bool drain_events();

}  // namespace vw::upower
