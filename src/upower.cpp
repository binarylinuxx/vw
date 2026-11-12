#include "vw/upower.hpp"

#include "dbus.hpp"

#include <mutex>

namespace vw::upower {
namespace {

constexpr const char* upower_name = "org.freedesktop.UPower";
constexpr const char* upower_path = "/org/freedesktop/UPower";
constexpr const char* device_interface = "org.freedesktop.UPower.Device";
// power-profiles-daemon moved under UPower's name in 0.20; try both.
struct ProfilesBus {
  const char* name;
  const char* path;
};
constexpr ProfilesBus profiles_buses[] = {
    {"org.freedesktop.UPower.PowerProfiles", "/org/freedesktop/UPower/PowerProfiles"},
    {"net.hadess.PowerProfiles", "/net/hadess/PowerProfiles"},
};

struct Snapshot {
  bool available = false;
  bool on_battery = false;
  std::optional<Device> display;
  std::vector<Device> devices;
  std::vector<std::string> profiles;
  std::string profile;
  // Which profiles bus answered.
  const ProfilesBus* profiles_bus = nullptr;
};

Device fetch_device(GDBusConnection* connection, const std::string& path) {
  Device device;
  device.path = path;
  auto properties = dbus::get_all(connection, upower_name, path.c_str(), device_interface);
  GVariant* map = properties.get();
  device.native_path = dbus::get_string(map, "NativePath");
  device.vendor = dbus::get_string(map, "Vendor");
  device.model = dbus::get_string(map, "Model");
  device.kind = static_cast<Kind>(dbus::get_int(map, "Type"));
  device.state = static_cast<State>(dbus::get_int(map, "State"));
  device.percentage = dbus::get_double(map, "Percentage");
  device.energy = dbus::get_double(map, "Energy");
  device.energy_full = dbus::get_double(map, "EnergyFull");
  device.energy_rate = dbus::get_double(map, "EnergyRate");
  device.time_to_empty = dbus::get_int(map, "TimeToEmpty");
  device.time_to_full = dbus::get_int(map, "TimeToFull");
  device.present = dbus::get_bool(map, "IsPresent");
  device.power_supply = dbus::get_bool(map, "PowerSupply");
  device.rechargeable = dbus::get_bool(map, "IsRechargeable");
  device.icon_name = dbus::get_string(map, "IconName");
  device.warning_level = static_cast<std::uint32_t>(dbus::get_int(map, "WarningLevel", 1));
  return device;
}

class Service {
 public:
  Service() : monitor_(G_BUS_TYPE_SYSTEM, [this](GDBusConnection* bus) { refresh(bus); }) {
    monitor_.watch(upower_name);
    for (const auto& bus : profiles_buses) monitor_.watch(bus.name);
  }

  Snapshot snapshot() {
    monitor_.start();
    std::lock_guard lock(mutex_);
    return snapshot_;
  }
  dbus::Monitor& monitor() {
    monitor_.start();
    return monitor_;
  }

 private:
  void refresh(GDBusConnection* bus) {
    Snapshot next;
    auto manager = dbus::get_all(bus, upower_name, upower_path, upower_name);
    next.available = static_cast<bool>(manager);
    next.on_battery = dbus::get_bool(manager.get(), "OnBattery");
    if (next.available) {
      if (auto display = dbus::call(bus, upower_name, upower_path, upower_name,
                                    "GetDisplayDevice", nullptr, "(o)")) {
        Device device = fetch_device(bus, dbus::string_of(
            dbus::adopt(g_variant_get_child_value(display.get(), 0)).get()));
        if (device.present) next.display = std::move(device);
      }
      if (auto list = dbus::call(bus, upower_name, upower_path, upower_name,
                                 "EnumerateDevices", nullptr, "(ao)")) {
        GVariantIter* iter = nullptr;
        const gchar* path = nullptr;
        g_variant_get(list.get(), "(ao)", &iter);
        while (g_variant_iter_next(iter, "&o", &path))
          next.devices.push_back(fetch_device(bus, path));
        g_variant_iter_free(iter);
      }
    }
    for (const auto& candidate : profiles_buses) {
      auto properties = dbus::get_all(bus, candidate.name, candidate.path, candidate.name);
      if (!properties) continue;
      next.profiles_bus = &candidate;
      next.profile = dbus::get_string(properties.get(), "ActiveProfile");
      auto list = dbus::lookup(properties.get(), "Profiles");
      if (list && g_variant_is_of_type(list.get(), G_VARIANT_TYPE("aa{sv}"))) {
        const gsize count = g_variant_n_children(list.get());
        for (gsize index = 0; index < count; ++index) {
          auto entry = dbus::adopt(g_variant_get_child_value(list.get(), index));
          next.profiles.push_back(dbus::get_string(entry.get(), "Profile"));
        }
      }
      break;
    }
    std::lock_guard lock(mutex_);
    snapshot_ = std::move(next);
  }

  std::mutex mutex_;
  Snapshot snapshot_;
  dbus::Monitor monitor_;
};

Service& service() {
  static Service instance;
  return instance;
}

}  // namespace

bool available() { return service().snapshot().available; }
bool on_battery() { return service().snapshot().on_battery; }
std::optional<Device> display_device() { return service().snapshot().display; }
std::vector<Device> devices() { return service().snapshot().devices; }
std::vector<std::string> profiles() { return service().snapshot().profiles; }
std::string profile() { return service().snapshot().profile; }

bool set_profile(std::string_view name) {
  const Snapshot snapshot = service().snapshot();
  if (snapshot.profiles_bus == nullptr) return false;
  const std::string value(name);
  const bool ok = dbus::set_property(service().monitor().connection(), snapshot.profiles_bus->name,
                                     snapshot.profiles_bus->path, snapshot.profiles_bus->name,
                                     "ActiveProfile", g_variant_new_string(value.c_str()));
  if (ok) service().monitor().schedule_refresh();
  return ok;
}

int event_fd() { return service().monitor().fd(); }
bool drain_events() { return service().monitor().drain(); }

}  // namespace vw::upower
