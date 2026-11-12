#include "dbus.hpp"

#include <stdexcept>
#include <utility>
#include <sys/eventfd.h>
#include <unistd.h>

namespace vw::dbus {

Variant lookup(GVariant* dictionary, const char* key) {
  if (dictionary == nullptr) return adopt(nullptr);
  Variant value = adopt(g_variant_lookup_value(dictionary, key, nullptr));
  if (value && g_variant_is_of_type(value.get(), G_VARIANT_TYPE_VARIANT))
    return adopt(g_variant_get_variant(value.get()));
  return value;
}

std::string string_of(GVariant* value) {
  if (value != nullptr && (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) ||
                           g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH) ||
                           g_variant_is_of_type(value, G_VARIANT_TYPE_SIGNATURE)))
    return g_variant_get_string(value, nullptr);
  return {};
}

std::int64_t int_of(GVariant* value, std::int64_t fallback) {
  if (value == nullptr) return fallback;
  switch (g_variant_classify(value)) {
    case G_VARIANT_CLASS_BYTE: return g_variant_get_byte(value);
    case G_VARIANT_CLASS_INT16: return g_variant_get_int16(value);
    case G_VARIANT_CLASS_UINT16: return g_variant_get_uint16(value);
    case G_VARIANT_CLASS_INT32: return g_variant_get_int32(value);
    case G_VARIANT_CLASS_UINT32: return g_variant_get_uint32(value);
    case G_VARIANT_CLASS_INT64: return g_variant_get_int64(value);
    case G_VARIANT_CLASS_UINT64: return static_cast<std::int64_t>(g_variant_get_uint64(value));
    default: return fallback;
  }
}

std::string bytes_of(GVariant* value) {
  if (value == nullptr || !g_variant_is_of_type(value, G_VARIANT_TYPE_BYTESTRING)) return {};
  gsize size = 0;
  const auto* data = static_cast<const char*>(
      g_variant_get_fixed_array(value, &size, sizeof(guchar)));
  std::string result(data, size);
  while (!result.empty() && result.back() == '\0') result.pop_back();
  return result;
}

std::string get_string(GVariant* dictionary, const char* key) {
  return string_of(lookup(dictionary, key).get());
}

bool get_bool(GVariant* dictionary, const char* key, bool fallback) {
  auto value = lookup(dictionary, key);
  if (!value || !g_variant_is_of_type(value.get(), G_VARIANT_TYPE_BOOLEAN)) return fallback;
  return g_variant_get_boolean(value.get()) != 0;
}

double get_double(GVariant* dictionary, const char* key, double fallback) {
  auto value = lookup(dictionary, key);
  if (!value) return fallback;
  if (g_variant_is_of_type(value.get(), G_VARIANT_TYPE_DOUBLE))
    return g_variant_get_double(value.get());
  return static_cast<double>(int_of(value.get(), static_cast<std::int64_t>(fallback)));
}

std::int64_t get_int(GVariant* dictionary, const char* key, std::int64_t fallback) {
  return int_of(lookup(dictionary, key).get(), fallback);
}

std::vector<std::string> get_strings(GVariant* dictionary, const char* key) {
  auto value = lookup(dictionary, key);
  std::vector<std::string> result;
  if (!value || !g_variant_is_of_type(value.get(), G_VARIANT_TYPE_ARRAY)) return result;
  const gsize count = g_variant_n_children(value.get());
  for (gsize index = 0; index < count; ++index) {
    Variant child = adopt(g_variant_get_child_value(value.get(), index));
    std::string text = string_of(child.get());
    if (!text.empty()) result.push_back(std::move(text));
  }
  return result;
}

Variant call(GDBusConnection* connection, const char* name, const char* path,
             const char* interface, const char* method, GVariant* arguments,
             const char* reply_type, int timeout_ms) {
  if (connection == nullptr) {
    if (arguments != nullptr) g_variant_unref(g_variant_ref_sink(arguments));
    return adopt(nullptr);
  }
  GError* error = nullptr;
  Variant reply = adopt(g_dbus_connection_call_sync(
      connection, name, path, interface, method, arguments,
      reply_type != nullptr ? G_VARIANT_TYPE(reply_type) : nullptr, G_DBUS_CALL_FLAGS_NONE,
      timeout_ms, nullptr, &error));
  if (error != nullptr) g_error_free(error);
  return reply;
}

Variant get_all(GDBusConnection* connection, const char* name, const char* path,
                const char* interface) {
  Variant reply = call(connection, name, path, "org.freedesktop.DBus.Properties", "GetAll",
                       g_variant_new("(s)", interface), "(a{sv})");
  if (!reply) return reply;
  return adopt(g_variant_get_child_value(reply.get(), 0));
}

bool set_property(GDBusConnection* connection, const char* name, const char* path,
                  const char* interface, const char* property, GVariant* value) {
  return static_cast<bool>(call(connection, name, path, "org.freedesktop.DBus.Properties", "Set",
                                g_variant_new("(ssv)", interface, property, value)));
}

Monitor::Monitor(GBusType type, Refresh refresh, Setup setup, unsigned debounce_ms)
    : type_(type), refresh_(std::move(refresh)), setup_(std::move(setup)),
      debounce_ms_(debounce_ms), event_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)),
      context_(g_main_context_new()) {
  if (event_ < 0) throw std::runtime_error("could not create service event fd");
}

Monitor::~Monitor() {
  if (thread_.joinable()) {
    invoke([this] {
      if (loop_ != nullptr) g_main_loop_quit(loop_);
    });
    thread_.join();
  }
  if (connection_ != nullptr) g_object_unref(connection_);
  g_main_context_unref(context_);
  close(event_);
}

void Monitor::watch(std::string name) { names_.push_back(std::move(name)); }

void Monitor::start() {
  std::unique_lock lock(mutex_);
  if (!started_) {
    started_ = true;
    thread_ = std::jthread([this] { run(); });
  }
  ready_condition_.wait(lock, [this] { return ready_; });
}

GDBusConnection* Monitor::connection() {
  std::lock_guard lock(mutex_);
  return connection_;
}

bool Monitor::drain() {
  std::uint64_t count = 0;
  bool changed = false;
  while (read(event_, &count, sizeof(count)) > 0) changed = true;
  return changed;
}

void Monitor::wake() {
  const std::uint64_t one = 1;
  const auto result = write(event_, &one, sizeof(one));
  (void)result;
}

void Monitor::invoke(std::function<void()> task) {
  if (connection() == nullptr) return;  // no bus, no loop
  auto* boxed = new std::function<void()>(std::move(task));
  g_main_context_invoke_full(
      context_, G_PRIORITY_DEFAULT,
      [](gpointer data) -> gboolean {
        (*static_cast<std::function<void()>*>(data))();
        return G_SOURCE_REMOVE;
      },
      boxed, [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
}

void Monitor::schedule_refresh() {
  invoke([this] {
    if (pending_ != nullptr) return;
    pending_ = g_timeout_source_new(debounce_ms_);
    g_source_set_callback(pending_, [](gpointer data) -> gboolean {
      auto* self = static_cast<Monitor*>(data);
      g_source_unref(self->pending_);
      self->pending_ = nullptr;
      self->refresh_(self->connection_);
      self->wake();
      return G_SOURCE_REMOVE;
    }, this, nullptr);
    g_source_attach(pending_, context_);
  });
}

void Monitor::run() {
  g_main_context_push_thread_default(context_);
  GError* error = nullptr;
  GDBusConnection* connection = g_bus_get_sync(type_, nullptr, &error);
  if (error != nullptr) g_error_free(error);
  {
    std::lock_guard lock(mutex_);
    connection_ = connection;
  }
  loop_ = g_main_loop_new(context_, FALSE);
  if (connection != nullptr) {
    const auto on_signal = [](GDBusConnection*, const gchar*, const gchar*, const gchar*,
                              const gchar*, GVariant*, gpointer data) {
      static_cast<Monitor*>(data)->schedule_refresh();
    };
    for (const auto& name : names_) {
      subscriptions_.push_back(g_dbus_connection_signal_subscribe(
          connection, name.c_str(), nullptr, nullptr, nullptr, nullptr,
          G_DBUS_SIGNAL_FLAGS_NONE, on_signal, this, nullptr));
      subscriptions_.push_back(g_dbus_connection_signal_subscribe(
          connection, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
          "/org/freedesktop/DBus", name.c_str(), G_DBUS_SIGNAL_FLAGS_NONE, on_signal, this,
          nullptr));
    }
    if (setup_) setup_(connection);
    refresh_(connection);
  }
  {
    std::lock_guard lock(mutex_);
    ready_ = true;
  }
  ready_condition_.notify_all();
  wake();
  if (connection != nullptr) g_main_loop_run(loop_);
  for (guint id : subscriptions_) g_dbus_connection_signal_unsubscribe(connection, id);
  if (pending_ != nullptr) {
    g_source_destroy(pending_);
    g_source_unref(pending_);
    pending_ = nullptr;
  }
  g_main_loop_unref(loop_);
  loop_ = nullptr;
  g_main_context_pop_thread_default(context_);
}

}  // namespace vw::dbus
