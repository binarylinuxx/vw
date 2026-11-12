#include "vw/bluetooth.hpp"

#include "dbus.hpp"

#include <algorithm>
#include <mutex>
#include <set>
#include <utility>

namespace vw::bluetooth {
namespace {

constexpr const char* bluez = "org.bluez";
constexpr const char* adapter_interface = "org.bluez.Adapter1";
constexpr const char* device_interface = "org.bluez.Device1";

struct Snapshot {
  bool available = false;
  std::vector<Adapter> adapters;
  std::vector<Device> devices;
};

class Service {
 public:
  Service() : monitor_(G_BUS_TYPE_SYSTEM, [this](GDBusConnection* bus) { refresh(bus); }) {
    monitor_.watch(bluez);
  }

  Snapshot snapshot() {
    monitor_.start();
    std::lock_guard lock(mutex_);
    Snapshot result = snapshot_;
    for (auto& device : result.devices) device.busy = busy_.contains(device.path);
    return result;
  }
  dbus::Monitor& monitor() {
    monitor_.start();
    return monitor_;
  }
  std::string last_error() {
    std::lock_guard lock(mutex_);
    return error_;
  }

  // Calls a Device1 method asynchronously on the monitor thread. then, when
  // set, runs there after success.
  void device_call(const std::string& path, const char* method,
                   std::function<void(GDBusConnection*)> then = {}) {
    {
      std::lock_guard lock(mutex_);
      busy_.insert(path);
      error_.clear();
    }
    monitor_.wake();
    struct Pending {
      Service* self;
      std::string path;
      std::function<void(GDBusConnection*)> then;
    };
    monitor_.invoke([this, path, method, then = std::move(then)]() mutable {
      g_dbus_connection_call(
          monitor_.connection(), bluez, path.c_str(), device_interface, method, nullptr, nullptr,
          G_DBUS_CALL_FLAGS_NONE, 60000, nullptr,
          [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<Pending> pending(static_cast<Pending*>(data));
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result,
                                                            &error);
            if (reply != nullptr) g_variant_unref(reply);
            Service& self = *pending->self;
            {
              std::lock_guard lock(self.mutex_);
              self.busy_.erase(pending->path);
              if (error != nullptr) {
                char* remote = g_dbus_error_get_remote_error(error);
                g_dbus_error_strip_remote_error(error);
                self.error_ = error->message;
                g_free(remote);
              }
            }
            if (error != nullptr) g_error_free(error);
            else if (pending->then) pending->then(G_DBUS_CONNECTION(source));
            self.monitor_.schedule_refresh();
            self.monitor_.wake();
          },
          new Pending{this, path, std::move(then)});
    });
  }

 private:
  void refresh(GDBusConnection* bus) {
    Snapshot next;
    auto reply = dbus::call(bus, bluez, "/", "org.freedesktop.DBus.ObjectManager",
                            "GetManagedObjects", nullptr, "(a{oa{sa{sv}}})");
    next.available = static_cast<bool>(reply);
    if (reply) {
      auto objects = dbus::adopt(g_variant_get_child_value(reply.get(), 0));
      GVariantIter iter;
      g_variant_iter_init(&iter, objects.get());
      const gchar* path = nullptr;
      GVariant* interfaces = nullptr;
      while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
        auto owned = dbus::adopt(interfaces);
        auto adapter = dbus::adopt(
            g_variant_lookup_value(interfaces, adapter_interface, G_VARIANT_TYPE("a{sv}")));
        if (adapter) {
          next.adapters.push_back({
              .path = path,
              .name = dbus::get_string(adapter.get(), "Alias"),
              .address = dbus::get_string(adapter.get(), "Address"),
              .powered = dbus::get_bool(adapter.get(), "Powered"),
              .discovering = dbus::get_bool(adapter.get(), "Discovering"),
              .discoverable = dbus::get_bool(adapter.get(), "Discoverable"),
          });
        }
        auto device = dbus::adopt(
            g_variant_lookup_value(interfaces, device_interface, G_VARIANT_TYPE("a{sv}")));
        if (!device) continue;
        Device entry{
            .path = path,
            .adapter = dbus::get_string(device.get(), "Adapter"),
            .name = dbus::get_string(device.get(), "Name"),
            .address = dbus::get_string(device.get(), "Address"),
            .icon = dbus::get_string(device.get(), "Icon"),
            .paired = dbus::get_bool(device.get(), "Paired"),
            .trusted = dbus::get_bool(device.get(), "Trusted"),
            .connected = dbus::get_bool(device.get(), "Connected"),
            .busy = false,
            .battery = std::nullopt,
        };
        // Alias falls back to the address for unnamed devices; keep those
        // only once paired.
        const std::string alias = dbus::get_string(device.get(), "Alias");
        if (entry.name.empty() && !entry.paired) continue;
        if (!alias.empty()) entry.name = alias;
        auto battery = dbus::adopt(
            g_variant_lookup_value(interfaces, "org.bluez.Battery1", G_VARIANT_TYPE("a{sv}")));
        if (battery) {
          const auto percentage = dbus::get_int(battery.get(), "Percentage", -1);
          if (percentage >= 0) entry.battery = static_cast<std::uint8_t>(percentage);
        }
        next.devices.push_back(std::move(entry));
      }
    }
    std::ranges::sort(next.adapters, {}, &Adapter::path);
    std::ranges::sort(next.devices, [](const Device& a, const Device& b) {
      if (a.connected != b.connected) return a.connected;
      if (a.paired != b.paired) return a.paired;
      return a.name < b.name;
    });
    std::lock_guard lock(mutex_);
    snapshot_ = std::move(next);
  }

  std::mutex mutex_;
  Snapshot snapshot_;
  std::set<std::string> busy_;
  std::string error_;
  dbus::Monitor monitor_;
};

Service& service() {
  static Service instance;
  return instance;
}

bool known(const std::string& path) {
  const auto devices = service().snapshot().devices;
  return std::ranges::find(devices, path, &Device::path) != devices.end();
}

bool adapter_call(const char* method) {
  const auto first = adapter();
  if (!first) return false;
  const bool ok = static_cast<bool>(dbus::call(service().monitor().connection(), bluez,
                                               first->path.c_str(), adapter_interface, method));
  if (ok) service().monitor().schedule_refresh();
  return ok;
}

}  // namespace

bool available() { return service().snapshot().available; }

std::optional<Adapter> adapter() {
  auto list = service().snapshot().adapters;
  if (list.empty()) return std::nullopt;
  return list.front();
}

std::vector<Adapter> adapters() { return service().snapshot().adapters; }
std::vector<Device> devices() { return service().snapshot().devices; }

bool set_powered(bool powered) {
  const auto first = adapter();
  if (!first) return false;
  const bool ok = dbus::set_property(service().monitor().connection(), bluez, first->path.c_str(),
                                     adapter_interface, "Powered",
                                     g_variant_new_boolean(powered ? TRUE : FALSE));
  if (ok) service().monitor().schedule_refresh();
  return ok;
}

bool set_discovering(bool discovering) {
  return adapter_call(discovering ? "StartDiscovery" : "StopDiscovery");
}

bool connect(const std::string& device) {
  if (!known(device)) return false;
  service().device_call(device, "Connect");
  return true;
}

bool disconnect(const std::string& device) {
  if (!known(device)) return false;
  service().device_call(device, "Disconnect");
  return true;
}

bool pair(const std::string& device) {
  if (!known(device)) return false;
  service().device_call(device, "Pair", [device](GDBusConnection* bus) {
    dbus::set_property(bus, bluez, device.c_str(), device_interface, "Trusted",
                       g_variant_new_boolean(TRUE));
    service().device_call(device, "Connect");
  });
  return true;
}

bool forget(const std::string& device) {
  const auto devices = service().snapshot().devices;
  const auto found = std::ranges::find(devices, device, &Device::path);
  if (found == devices.end()) return false;
  const bool ok = static_cast<bool>(dbus::call(
      service().monitor().connection(), bluez, found->adapter.c_str(), adapter_interface,
      "RemoveDevice", g_variant_new("(o)", device.c_str())));
  if (ok) service().monitor().schedule_refresh();
  return ok;
}

std::string last_error() { return service().last_error(); }
int event_fd() { return service().monitor().fd(); }
bool drain_events() { return service().monitor().drain(); }

}  // namespace vw::bluetooth
