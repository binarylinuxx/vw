#include "vw/network.hpp"

#include "dbus.hpp"

#include <algorithm>
#include <mutex>
#include <set>
#include <unordered_map>

namespace vw::network {
namespace {

constexpr const char* nm_name = "org.freedesktop.NetworkManager";
constexpr const char* nm_path = "/org/freedesktop/NetworkManager";
constexpr const char* device_interface = "org.freedesktop.NetworkManager.Device";
constexpr const char* wireless_interface = "org.freedesktop.NetworkManager.Device.Wireless";
constexpr const char* ap_interface = "org.freedesktop.NetworkManager.AccessPoint";
constexpr const char* active_interface = "org.freedesktop.NetworkManager.Connection.Active";
constexpr const char* settings_path = "/org/freedesktop/NetworkManager/Settings";
constexpr const char* settings_interface = "org.freedesktop.NetworkManager.Settings";
constexpr const char* connection_interface = "org.freedesktop.NetworkManager.Settings.Connection";
constexpr std::uint32_t device_ethernet = 1;
constexpr std::uint32_t device_wifi = 2;
constexpr std::uint32_t device_activated = 100;

struct Snapshot {
  Status status;
  std::vector<AccessPoint> access_points;
  // For actions: the Wi-Fi device, the AP object per SSID, and the saved
  // connection per SSID.
  std::string wifi_device;
  std::string wifi_active_connection;
  std::unordered_map<std::string, std::string> ap_paths;
  std::unordered_map<std::string, std::string> saved;
};

Kind kind_of(const std::string& type) {
  if (type.empty()) return Kind::none;
  if (type == "802-3-ethernet") return Kind::wired;
  if (type == "802-11-wireless") return Kind::wifi;
  if (type == "vpn" || type == "wireguard") return Kind::vpn;
  return Kind::other;
}

std::vector<std::string> object_paths(const dbus::Variant& reply) {
  std::vector<std::string> paths;
  if (!reply) return paths;
  GVariantIter* iter = nullptr;
  const gchar* path = nullptr;
  g_variant_get(reply.get(), "(ao)", &iter);
  while (g_variant_iter_next(iter, "&o", &path)) paths.emplace_back(path);
  g_variant_iter_free(iter);
  return paths;
}

// SSID → settings object of every saved Wi-Fi connection.
std::unordered_map<std::string, std::string> saved_connections(GDBusConnection* bus) {
  std::unordered_map<std::string, std::string> saved;
  const auto paths = object_paths(dbus::call(bus, nm_name, settings_path, settings_interface,
                                             "ListConnections", nullptr, "(ao)"));
  for (const auto& path : paths) {
    auto reply = dbus::call(bus, nm_name, path.c_str(), connection_interface, "GetSettings",
                            nullptr, "(a{sa{sv}})");
    if (!reply) continue;
    auto settings = dbus::adopt(g_variant_get_child_value(reply.get(), 0));
    auto wireless = dbus::adopt(
        g_variant_lookup_value(settings.get(), "802-11-wireless", G_VARIANT_TYPE("a{sv}")));
    if (!wireless) continue;
    const std::string ssid = dbus::bytes_of(dbus::lookup(wireless.get(), "ssid").get());
    if (!ssid.empty()) saved.emplace(ssid, path);
  }
  return saved;
}

class Service {
 public:
  Service()
      : monitor_(G_BUS_TYPE_SYSTEM, [this](GDBusConnection* bus) { refresh(bus); }, {}, 150) {
    monitor_.watch(nm_name);
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
    Status& status = next.status;
    auto manager = dbus::get_all(bus, nm_name, nm_path, nm_name);
    status.available = static_cast<bool>(manager);
    if (!status.available) {
      std::lock_guard lock(mutex_);
      snapshot_ = std::move(next);
      return;
    }
    status.networking_enabled = dbus::get_bool(manager.get(), "NetworkingEnabled");
    status.wifi_enabled = dbus::get_bool(manager.get(), "WirelessEnabled");
    status.wifi_hardware_enabled = dbus::get_bool(manager.get(), "WirelessHardwareEnabled");
    status.connectivity = static_cast<Connectivity>(dbus::get_int(manager.get(), "Connectivity"));
    status.primary = kind_of(dbus::get_string(manager.get(), "PrimaryConnectionType"));

    const std::string primary = dbus::get_string(manager.get(), "PrimaryConnection");
    if (primary.size() > 1) {
      auto active = dbus::get_all(bus, nm_name, primary.c_str(), active_interface);
      status.connection = dbus::get_string(active.get(), "Id");
      const std::string ip4 = dbus::get_string(active.get(), "Ip4Config");
      if (ip4.size() > 1) {
        auto config = dbus::get_all(bus, nm_name, ip4.c_str(),
                                    "org.freedesktop.NetworkManager.IP4Config");
        auto addresses = dbus::lookup(config.get(), "AddressData");
        if (addresses && g_variant_is_of_type(addresses.get(), G_VARIANT_TYPE("aa{sv}")) &&
            g_variant_n_children(addresses.get()) > 0) {
          auto first = dbus::adopt(g_variant_get_child_value(addresses.get(), 0));
          status.address = dbus::get_string(first.get(), "address");
          if (const auto prefix = dbus::get_int(first.get(), "prefix", -1); prefix >= 0)
            status.address += "/" + std::to_string(prefix);
        }
      }
    }

    std::string active_ap;
    for (const auto& path : object_paths(dbus::call(bus, nm_name, nm_path, nm_name, "GetDevices",
                                                    nullptr, "(ao)"))) {
      auto device = dbus::get_all(bus, nm_name, path.c_str(), device_interface);
      const auto type = dbus::get_int(device.get(), "DeviceType");
      const auto state = dbus::get_int(device.get(), "State");
      if (type == device_ethernet && state == device_activated) status.wired = true;
      if (type != device_wifi || !next.wifi_device.empty()) continue;
      status.has_wifi = true;
      next.wifi_device = path;
      const std::string active = dbus::get_string(device.get(), "ActiveConnection");
      if (active.size() > 1) next.wifi_active_connection = active;
      auto wireless = dbus::get_all(bus, nm_name, path.c_str(), wireless_interface);
      active_ap = dbus::get_string(wireless.get(), "ActiveAccessPoint");
      for (const auto& ap_path : dbus::get_strings(wireless.get(), "AccessPoints")) {
        auto ap = dbus::get_all(bus, nm_name, ap_path.c_str(), ap_interface);
        if (!ap) continue;
        AccessPoint point;
        point.ssid = dbus::bytes_of(dbus::lookup(ap.get(), "Ssid").get());
        if (point.ssid.empty()) continue;
        point.bssid = dbus::get_string(ap.get(), "HwAddress");
        point.strength = static_cast<std::uint8_t>(dbus::get_int(ap.get(), "Strength"));
        point.frequency = static_cast<std::uint32_t>(dbus::get_int(ap.get(), "Frequency"));
        point.secured = (dbus::get_int(ap.get(), "Flags") & 1) != 0 ||
                        dbus::get_int(ap.get(), "WpaFlags") != 0 ||
                        dbus::get_int(ap.get(), "RsnFlags") != 0;
        point.active = ap_path == active_ap;
        // Keep the strongest AP per SSID, but always the active one.
        auto existing = std::ranges::find(next.access_points, point.ssid, &AccessPoint::ssid);
        if (existing == next.access_points.end()) {
          next.ap_paths[point.ssid] = ap_path;
          next.access_points.push_back(std::move(point));
        } else if (point.active || (!existing->active && point.strength > existing->strength)) {
          next.ap_paths[point.ssid] = ap_path;
          *existing = std::move(point);
        }
      }
    }

    next.saved = saved_connections(bus);
    for (auto& point : next.access_points) {
      point.saved = next.saved.contains(point.ssid);
      if (point.active) {
        status.ssid = point.ssid;
        status.strength = point.strength;
      }
    }
    std::ranges::sort(next.access_points, [](const AccessPoint& a, const AccessPoint& b) {
      if (a.active != b.active) return a.active;
      return a.strength > b.strength;
    });
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

GDBusConnection* bus() { return service().monitor().connection(); }

bool done(bool ok) {
  if (ok) service().monitor().schedule_refresh();
  return ok;
}

}  // namespace

Status status() { return service().snapshot().status; }
std::vector<AccessPoint> access_points() { return service().snapshot().access_points; }

std::string icon_name(const Status& status) {
  if (!status.available || !status.networking_enabled) return "network-offline-symbolic";
  switch (status.primary) {
    case Kind::wired: return "network-wired-symbolic";
    case Kind::vpn: return "network-vpn-symbolic";
    case Kind::wifi: {
      const char* level = status.strength >= 80 ? "excellent"
                          : status.strength >= 55 ? "good"
                          : status.strength >= 30 ? "ok"
                          : status.strength >= 5  ? "weak"
                                                  : "none";
      return std::string("network-wireless-signal-") + level + "-symbolic";
    }
    case Kind::other: return "network-transmit-receive-symbolic";
    case Kind::none: break;
  }
  if (status.has_wifi && !status.wifi_enabled) return "network-wireless-disabled-symbolic";
  return status.has_wifi ? "network-wireless-offline-symbolic" : "network-wired-disconnected-symbolic";
}

bool set_wifi_enabled(bool enabled) {
  return done(dbus::set_property(bus(), nm_name, nm_path, nm_name, "WirelessEnabled",
                                 g_variant_new_boolean(enabled ? TRUE : FALSE)));
}

bool scan() {
  const Snapshot snapshot = service().snapshot();
  if (snapshot.wifi_device.empty()) return false;
  GVariantBuilder options;
  g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
  return static_cast<bool>(dbus::call(bus(), nm_name, snapshot.wifi_device.c_str(),
                                      wireless_interface, "RequestScan",
                                      g_variant_new("(a{sv})", &options)));
}

bool connect(std::string_view ssid_view, std::string_view password) {
  const Snapshot snapshot = service().snapshot();
  if (snapshot.wifi_device.empty()) return false;
  const std::string ssid(ssid_view);
  const auto ap = snapshot.ap_paths.find(ssid);
  const std::string specific = ap != snapshot.ap_paths.end() ? ap->second : "/";
  if (const auto saved = snapshot.saved.find(ssid); saved != snapshot.saved.end() && password.empty())
    return done(static_cast<bool>(dbus::call(
        bus(), nm_name, nm_path, nm_name, "ActivateConnection",
        g_variant_new("(ooo)", saved->second.c_str(), snapshot.wifi_device.c_str(),
                      specific.c_str()),
        "(o)", 10000)));

  GVariantBuilder settings;
  g_variant_builder_init(&settings, G_VARIANT_TYPE("a{sa{sv}}"));
  GVariantBuilder wireless;
  g_variant_builder_init(&wireless, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&wireless, "{sv}", "ssid",
                        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, ssid.data(), ssid.size(), 1));
  g_variant_builder_add(&settings, "{sa{sv}}", "802-11-wireless", &wireless);
  if (!password.empty()) {
    const std::string psk(password);
    GVariantBuilder security;
    g_variant_builder_init(&security, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&security, "{sv}", "key-mgmt", g_variant_new_string("wpa-psk"));
    g_variant_builder_add(&security, "{sv}", "psk", g_variant_new_string(psk.c_str()));
    g_variant_builder_add(&settings, "{sa{sv}}", "802-11-wireless-security", &security);
  }
  return done(static_cast<bool>(dbus::call(
      bus(), nm_name, nm_path, nm_name, "AddAndActivateConnection",
      g_variant_new("(a{sa{sv}}oo)", &settings, snapshot.wifi_device.c_str(), specific.c_str()),
      "(oo)", 10000)));
}

bool disconnect_wifi() {
  const Snapshot snapshot = service().snapshot();
  if (snapshot.wifi_active_connection.empty()) return false;
  return done(static_cast<bool>(dbus::call(bus(), nm_name, nm_path, nm_name,
                                           "DeactivateConnection",
                                           g_variant_new("(o)",
                                                         snapshot.wifi_active_connection.c_str()))));
}

int event_fd() { return service().monitor().fd(); }
bool drain_events() { return service().monitor().drain(); }

}  // namespace vw::network
