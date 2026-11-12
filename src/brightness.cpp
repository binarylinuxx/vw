#include "vw/brightness.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sys/inotify.h>
#include <unistd.h>

namespace vw::brightness {
namespace {

namespace fs = std::filesystem;
constexpr const char* sysfs = "/sys/class";

std::uint32_t read_number(const fs::path& path) {
  std::ifstream input(path);
  std::uint64_t value = 0;
  input >> value;
  return static_cast<std::uint32_t>(value);
}

std::string read_word(const fs::path& path) {
  std::ifstream input(path);
  std::string value;
  input >> value;
  return value;
}

std::optional<Backlight> load(const std::string& subsystem, const std::string& name) {
  const fs::path directory = fs::path(sysfs) / subsystem / name;
  Backlight backlight{.subsystem = subsystem, .name = name,
                      .brightness = read_number(directory / "brightness"),
                      .max_brightness = read_number(directory / "max_brightness")};
  if (backlight.max_brightness == 0) return std::nullopt;
  return backlight;
}

std::vector<std::string> entries(const std::string& subsystem) {
  std::vector<std::string> names;
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(fs::path(sysfs) / subsystem, error))
    names.push_back(entry.path().filename().string());
  std::ranges::sort(names);
  return names;
}

// Watches every brightness file; sysfs reports writes made from user space.
class Watcher {
 public:
  Watcher() : fd_(inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) {
    if (fd_ < 0) return;
    for (const auto& backlight : all()) {
      const auto file = fs::path(sysfs) / backlight.subsystem / backlight.name / "brightness";
      inotify_add_watch(fd_, file.c_str(), IN_MODIFY);
    }
  }
  ~Watcher() {
    if (fd_ >= 0) close(fd_);
  }
  int fd() const { return fd_; }
  bool drain() const {
    alignas(inotify_event) char buffer[4096];
    bool changed = false;
    while (fd_ >= 0 && read(fd_, buffer, sizeof(buffer)) > 0) changed = true;
    return changed;
  }

 private:
  int fd_;
};

Watcher& watcher() {
  static Watcher instance;
  return instance;
}

bool logind_set(const Backlight& backlight, std::uint32_t value) {
  GError* error = nullptr;
  GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
  if (bus == nullptr) {
    g_error_free(error);
    return false;
  }
  GVariant* reply = g_dbus_connection_call_sync(
      bus, "org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
      "org.freedesktop.login1.Session", "SetBrightness",
      g_variant_new("(ssu)", backlight.subsystem.c_str(), backlight.name.c_str(), value),
      nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
  g_object_unref(bus);
  if (reply == nullptr) {
    g_error_free(error);
    return false;
  }
  g_variant_unref(reply);
  return true;
}

}  // namespace

std::vector<Backlight> all() {
  std::vector<Backlight> result;
  for (const auto& name : entries("backlight"))
    if (auto backlight = load("backlight", name)) result.push_back(std::move(*backlight));
  for (const auto& name : entries("leds"))
    if (name.find("kbd_backlight") != std::string::npos)
      if (auto backlight = load("leds", name)) result.push_back(std::move(*backlight));
  return result;
}

std::optional<Backlight> screen() {
  std::optional<Backlight> best;
  int best_rank = -1;
  for (const auto& name : entries("backlight")) {
    const std::string type = read_word(fs::path(sysfs) / "backlight" / name / "type");
    const int rank = type == "firmware" ? 3 : type == "platform" ? 2 : type == "raw" ? 1 : 0;
    if (rank <= best_rank) continue;
    if (auto backlight = load("backlight", name)) {
      best = std::move(backlight);
      best_rank = rank;
    }
  }
  return best;
}

std::optional<Backlight> keyboard() {
  for (const auto& name : entries("leds"))
    if (name.find("kbd_backlight") != std::string::npos)
      if (auto backlight = load("leds", name)) return backlight;
  return std::nullopt;
}

bool set(const Backlight& backlight, double fraction) {
  if (backlight.max_brightness == 0) return false;
  const auto value = static_cast<std::uint32_t>(
      std::lround(std::clamp(fraction, 0.0, 1.0) * backlight.max_brightness));
  if (logind_set(backlight, value)) return true;
  std::ofstream output(fs::path(sysfs) / backlight.subsystem / backlight.name / "brightness");
  output << value;
  return static_cast<bool>(output.flush());
}

bool set_screen(double fraction) {
  const auto backlight = screen();
  return backlight && set(*backlight, fraction);
}

bool change_screen(double delta) {
  const auto backlight = screen();
  return backlight && set(*backlight, backlight->fraction() + delta);
}

int event_fd() { return watcher().fd(); }
bool drain_events() { return watcher().drain(); }

}  // namespace vw::brightness
