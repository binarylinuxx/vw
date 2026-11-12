#pragma once

#include <gio/gio.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Internal D-Bus helpers shared by the system services (UPower, tray,
// NetworkManager, BlueZ). Not installed.
namespace vw::dbus {

using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
inline Variant adopt(GVariant* value) { return {value, &g_variant_unref}; }

// Looks key up in an a{sv} dictionary, unwrapping the variant; null when
// missing.
Variant lookup(GVariant* dictionary, const char* key);
// s, o and g values; empty otherwise.
std::string get_string(GVariant* dictionary, const char* key);
bool get_bool(GVariant* dictionary, const char* key, bool fallback = false);
double get_double(GVariant* dictionary, const char* key, double fallback = 0);
// Any integer type.
std::int64_t get_int(GVariant* dictionary, const char* key, std::int64_t fallback = 0);
// as and ao.
std::vector<std::string> get_strings(GVariant* dictionary, const char* key);
std::string string_of(GVariant* value);
std::int64_t int_of(GVariant* value, std::int64_t fallback = 0);
// An ay value, e.g. a Wi-Fi SSID, as a string without trailing NULs.
std::string bytes_of(GVariant* value);

// Synchronous call; null on any failure. Consumes a floating arguments.
Variant call(GDBusConnection* connection, const char* name, const char* path,
             const char* interface, const char* method, GVariant* arguments = nullptr,
             const char* reply_type = nullptr, int timeout_ms = 1000);
// org.freedesktop.DBus.Properties.GetAll; the a{sv} dictionary or null.
Variant get_all(GDBusConnection* connection, const char* name, const char* path,
                const char* interface);
bool set_property(GDBusConnection* connection, const char* name, const char* path,
                  const char* interface, const char* property, GVariant* value);

// A GLib main loop on its own thread that keeps one service's state fresh.
// Signals from a watched bus name, and the name appearing or vanishing,
// schedule a debounced refresh on that thread; after every refresh the event
// fd becomes readable so a vw::App rebuilds.
class Monitor {
 public:
  using Refresh = std::function<void(GDBusConnection*)>;
  using Setup = std::function<void(GDBusConnection*)>;

  // setup runs once on the monitor thread before the first refresh, e.g. to
  // own names or export objects.
  Monitor(GBusType type, Refresh refresh, Setup setup = {}, unsigned debounce_ms = 60);
  ~Monitor();
  Monitor(const Monitor&) = delete;
  Monitor& operator=(const Monitor&) = delete;

  // Refresh on every signal from name. Only before start().
  void watch(std::string name);
  // Starts the thread and waits for the first refresh. Idempotent.
  void start();
  // The bus connection, or null when the bus is unreachable.
  GDBusConnection* connection();
  int fd() const { return event_; }
  bool drain();
  void wake();
  void schedule_refresh();
  // Runs task on the monitor thread (immediately when already on it).
  void invoke(std::function<void()> task);

 private:
  void run();

  GBusType type_;
  Refresh refresh_;
  Setup setup_;
  unsigned debounce_ms_;
  int event_ = -1;
  GMainContext* context_ = nullptr;
  GMainLoop* loop_ = nullptr;
  GSource* pending_ = nullptr;
  GDBusConnection* connection_ = nullptr;
  std::vector<std::string> names_;
  std::vector<guint> subscriptions_;
  std::mutex mutex_;
  std::condition_variable ready_condition_;
  bool started_ = false;
  bool ready_ = false;
  std::jthread thread_;
};

}  // namespace vw::dbus
