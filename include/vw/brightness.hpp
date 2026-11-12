#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Screen and keyboard backlights from sysfs. Changes are written through
// systemd-logind, so no root or udev rule is needed, falling back to writing
// sysfs directly.
namespace vw::brightness {

struct Backlight {
  // "backlight" for screens, "leds" for keyboard backlights.
  std::string subsystem;
  // e.g. "intel_backlight", "amdgpu_bl1", "tpacpi::kbd_backlight".
  std::string name;
  std::uint32_t brightness = 0;
  std::uint32_t max_brightness = 0;

  // 0 to 1.
  double fraction() const {
    return max_brightness == 0 ? 0.0 : static_cast<double>(brightness) / max_brightness;
  }
};

// The screen backlight: firmware interfaces first, then platform, then raw,
// as the kernel documentation recommends. nullopt on desktops without one.
std::optional<Backlight> screen();
std::optional<Backlight> keyboard();
std::vector<Backlight> all();

// fraction is clamped to 0..1. Returns false when nothing could be written.
bool set(const Backlight& backlight, double fraction);
// Shorthands for the screen backlight.
bool set_screen(double fraction);
bool change_screen(double delta);

// Readable when a brightness file is written (by vw, logind or another tool).
// Changes made by firmware hotkeys are not always reported.
int event_fd();
bool drain_events();

}  // namespace vw::brightness
