#include "vw/notifications.hpp"

#include "vw/icon.hpp"

#include <cairo.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sys/eventfd.h>
#include <unistd.h>

#ifndef VW_VERSION
#define VW_VERSION "unknown"
#endif

namespace vw::notifications {
namespace {

constexpr const char* bus_name = "org.freedesktop.Notifications";
constexpr const char* object_path = "/org/freedesktop/Notifications";
constexpr const char* interface_xml = R"XML(
<node><interface name="org.freedesktop.Notifications">
  <method name="GetCapabilities"><arg type="as" direction="out"/></method>
  <method name="GetServerInformation">
    <arg type="s" direction="out"/><arg type="s" direction="out"/>
    <arg type="s" direction="out"/><arg type="s" direction="out"/>
  </method>
  <method name="Notify">
    <arg name="app_name" type="s" direction="in"/><arg name="replaces_id" type="u" direction="in"/>
    <arg name="app_icon" type="s" direction="in"/><arg name="summary" type="s" direction="in"/>
    <arg name="body" type="s" direction="in"/><arg name="actions" type="as" direction="in"/>
    <arg name="hints" type="a{sv}" direction="in"/><arg name="expire_timeout" type="i" direction="in"/>
    <arg name="id" type="u" direction="out"/>
  </method>
  <method name="CloseNotification"><arg name="id" type="u" direction="in"/></method>
  <signal name="NotificationClosed"><arg name="id" type="u"/><arg name="reason" type="u"/></signal>
  <signal name="ActionInvoked"><arg name="id" type="u"/><arg name="action_key" type="s"/></signal>
</interface></node>)XML";

// Spec close reasons.
constexpr std::uint32_t expired = 1;
constexpr std::uint32_t dismissed = 2;
constexpr std::uint32_t withdrawn = 3;
constexpr std::uint32_t evicted = 4;

constexpr int default_timeout_ms = 5000;
constexpr std::size_t history_limit = 256;
constexpr std::size_t open_limit = 256;

using Clock = std::chrono::steady_clock;
using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;
Variant adopt(GVariant* value) { return {value, &g_variant_unref}; }

// --- Hints ---------------------------------------------------------------------

class Hints {
 public:
  explicit Hints(GVariant* dictionary) : dictionary_(dictionary) {}

  Variant get(const char* key) const {
    Variant value = adopt(g_variant_lookup_value(dictionary_, key, nullptr));
    if (value && g_variant_is_of_type(value.get(), G_VARIANT_TYPE_VARIANT))
      return adopt(g_variant_get_variant(value.get()));
    return value;
  }
  // The first of `keys` present as a string.
  std::string string(std::initializer_list<const char*> keys) const {
    for (const char* key : keys)
      if (auto value = get(key); value && g_variant_is_of_type(value.get(), G_VARIANT_TYPE_STRING))
        return g_variant_get_string(value.get(), nullptr);
    return {};
  }
  bool boolean(const char* key) const {
    auto value = get(key);
    return value && g_variant_is_of_type(value.get(), G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(value.get());
  }
  // Senders disagree on integer types (urgency is a byte, value an int).
  std::optional<std::int64_t> integer(const char* key) const {
    auto value = get(key);
    if (!value) return std::nullopt;
    GVariant* v = value.get();
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_BYTE)) return g_variant_get_byte(v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT16)) return g_variant_get_int16(v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT16)) return g_variant_get_uint16(v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32)) return g_variant_get_int32(v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32)) return g_variant_get_uint32(v);
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64)) return g_variant_get_int64(v);
    return std::nullopt;
  }

 private:
  GVariant* dictionary_;
};

// --- Icons and images ----------------------------------------------------------

// A path, a file:// URI or a theme name; empty for a missing file.
std::string icon_source(const std::string& value) {
  std::string result = value;
  if (value.starts_with("file://")) {
    char* path = g_filename_from_uri(value.c_str(), nullptr, nullptr);
    result = path == nullptr ? "" : path;
    g_free(path);
  }
  std::error_code error;
  if (g_path_is_absolute(result.c_str()) && !std::filesystem::exists(result, error)) return {};
  return result;
}

std::string desktop_icon(const std::string& entry) {
  if (entry.empty()) return {};
  const std::string id = entry.ends_with(".desktop") ? entry : entry + ".desktop";
  GDesktopAppInfo* info = g_desktop_app_info_new(id.c_str());
  if (info == nullptr) return {};
  GIcon* icon = g_app_info_get_icon(G_APP_INFO(info));
  char* text = icon == nullptr ? nullptr : g_icon_to_string(icon);
  std::string result = text == nullptr ? "" : text;
  g_free(text);
  g_object_unref(info);
  return result;
}

// Only the daemon holding the bus name writes here, so anything found on
// first use was left by an earlier run.
const std::filesystem::path& image_directory() {
  static const std::filesystem::path directory = [] {
    std::filesystem::path path = std::filesystem::path(g_get_user_runtime_dir()) / "vw" / "notifications";
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path, error);
    return path;
  }();
  return directory;
}

// Writes an image-data hint, (iiibiiay): width, height, rowstride, has alpha,
// bits per sample, channels, pixels. The PNG's path, or empty when malformed.
std::string write_image(GVariant* value) {
  if (!g_variant_is_of_type(value, G_VARIANT_TYPE("(iiibiiay)"))) return {};
  gint width = 0, height = 0, rowstride = 0, bits = 0, channels = 0;
  gboolean has_alpha = FALSE;
  GVariant* raw = nullptr;
  g_variant_get(value, "(iiibii@ay)", &width, &height, &rowstride, &has_alpha, &bits, &channels, &raw);
  Variant data = adopt(raw);
  gsize size = 0;
  const auto* pixels = static_cast<const std::uint8_t*>(g_variant_get_fixed_array(data.get(), &size, 1));
  constexpr int limit = 4096;
  if (width <= 0 || height <= 0 || width > limit || height > limit || bits != 8 || channels != (has_alpha ? 4 : 3))
    return {};
  const auto row = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(channels);
  if (rowstride < 0 || static_cast<std::uint64_t>(rowstride) < row ||
      static_cast<std::uint64_t>(rowstride) * static_cast<std::uint64_t>(height - 1) + row > size)
    return {};

  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    return {};
  }
  cairo_surface_flush(surface);
  std::uint8_t* target = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  for (int y = 0; y < height; ++y) {
    const std::uint8_t* source = pixels + static_cast<std::ptrdiff_t>(y) * rowstride;
    auto* line = reinterpret_cast<std::uint32_t*>(target + static_cast<std::ptrdiff_t>(y) * stride);
    for (int x = 0; x < width; ++x, source += channels) {
      const std::uint32_t alpha = has_alpha ? source[3] : 255U;
      const auto scale = [alpha](std::uint32_t channel) { return (channel * alpha + 127U) / 255U; };
      line[x] = alpha << 24U | scale(source[0]) << 16U | scale(source[1]) << 8U | scale(source[2]);
    }
  }
  cairo_surface_mark_dirty(surface);
  static std::uint64_t next = 0;  // D-Bus thread only
  const auto path = image_directory() / std::format("{}-{}.png", getpid(), ++next);
  const cairo_status_t status = cairo_surface_write_to_png(surface, path.c_str());
  cairo_surface_destroy(surface);
  return status == CAIRO_STATUS_SUCCESS ? path.string() : "";
}

void remove_file(const std::string& path) {
  if (path.empty()) return;
  std::error_code error;
  std::filesystem::remove(path, error);
}

// --- Model -----------------------------------------------------------------------

struct Record : Notification {
  Clock::time_point expires{};
  // Paths or theme names; resolved into icon and image by readers.
  std::string icon_source;
  std::string image_source;
  // The PNG written for image-data, removed with the record.
  std::string owned_image;
  // Synchronous / stack tag: a later notification with the same tag from
  // the same app replaces this one in place.
  std::string tag;
  // In the history (see the header).
  bool kept = true;
};

enum class View : std::size_t { open, popups, history };

struct Signal {
  const char* name;
  std::uint32_t id;
  std::uint32_t reason;  // NotificationClosed
  std::string key;       // ActionInvoked
};

class Service {
 public:
  Service() : event_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)), context_(g_main_context_new()) {
    if (event_ < 0) throw std::runtime_error("could not create notification event fd");
  }
  ~Service() {
    if (thread_.joinable()) {
      thread_.request_stop();
      g_main_context_invoke(context_, [](gpointer data) -> gboolean {
        auto* self = static_cast<Service*>(data);
        if (self->loop_ != nullptr) g_main_loop_quit(self->loop_);
        return G_SOURCE_REMOVE;
      }, this);
      thread_.join();
    }
    for (const Record& record : records_) remove_file(record.owned_image);
    g_main_context_unref(context_);
    close(event_);
  }
  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  StartResult start() {
    std::unique_lock lock(mutex_);
    if (!started_) {
      started_ = true;
      thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    ready_condition_.wait(lock, [this] { return ready_; });
    return {running_, error_};
  }
  bool running() {
    std::lock_guard lock(mutex_);
    return running_;
  }
  int fd() const { return event_; }
  bool drain() {
    std::uint64_t count = 0;
    bool changed = false;
    while (read(event_, &count, sizeof(count)) > 0) changed = true;
    return changed;
  }
  std::uint64_t generation() {
    std::lock_guard lock(mutex_);
    return generation_;
  }

  // Rebuilt only when the model changed since the last call for this view.
  std::vector<Notification> snapshot(View view) {
    std::lock_guard cache_lock(cache_mutex_);
    Cache& cache = caches_[static_cast<std::size_t>(view)];
    std::vector<Record> matching;
    {
      std::lock_guard lock(mutex_);
      if (cache.generation == generation_) return cache.items;
      cache.generation = generation_;
      for (auto record = records_.rbegin(); record != records_.rend(); ++record) {
        const bool wanted = view == View::open     ? !record->closed
                            : view == View::popups ? record->popup && !record->closed
                                                   : record->kept;
        if (wanted) matching.push_back(*record);
      }
    }
    // Theme lookups happen here, on the reader's thread, once per source.
    if (resolved_.size() > 1024) resolved_.clear();
    const auto resolve = [this](const std::string& source) -> std::string {
      if (source.empty()) return {};
      auto found = resolved_.find(source);
      if (found == resolved_.end()) found = resolved_.emplace(source, resolve_icon(source).string()).first;
      return found->second;
    };
    cache.items.clear();
    cache.items.reserve(matching.size());
    for (Record& record : matching) {
      record.icon = resolve(record.icon_source);
      record.image = resolve(record.image_source);
      cache.items.push_back(std::move(static_cast<Notification&>(record)));
    }
    return cache.items;
  }

  bool dismiss(std::uint32_t id) {
    std::vector<Signal> signals;
    bool found = false;
    {
      std::lock_guard lock(mutex_);
      if (Record* record = find(id)) {
        found = true;
        record->kept = false;
        record->popup = false;
        close_locked(*record, dismissed, signals);
        prune_locked();
        changed_locked();
      }
    }
    emit(signals);
    return found;
  }
  void hide_popup(std::uint32_t id) {
    std::lock_guard lock(mutex_);
    if (Record* record = find(id); record != nullptr && record->popup) {
      record->popup = false;
      changed_locked();
    }
  }
  void hide_popups() {
    std::lock_guard lock(mutex_);
    hide_popups_locked();
  }
  bool invoke(std::uint32_t id, const std::string& key) {
    std::vector<Signal> signals;
    {
      std::lock_guard lock(mutex_);
      Record* record = find(id);
      if (record == nullptr || record->closed || std::ranges::find(record->actions, key, &Action::key) == record->actions.end())
        return false;
      signals.push_back({"ActionInvoked", id, 0, key});
      if (!record->resident) {
        record->kept = false;
        record->popup = false;
        close_locked(*record, dismissed, signals);
        prune_locked();
      }
      changed_locked();
    }
    emit(signals);
    return true;
  }
  bool activate(std::uint32_t id) {
    std::string key;
    {
      std::lock_guard lock(mutex_);
      const Record* record = find(id);
      if (record == nullptr || record->closed) return false;
      if (std::ranges::find(record->actions, "default", &Action::key) != record->actions.end()) key = "default";
      else if (record->actions.size() == 1) key = record->actions.front().key;
      else return false;
    }
    return invoke(id, key);
  }
  void clear() {
    std::vector<Signal> signals;
    {
      std::lock_guard lock(mutex_);
      for (Record& record : records_) {
        record.kept = false;
        record.popup = false;
        close_locked(record, dismissed, signals);
      }
      prune_locked();
      changed_locked();
    }
    emit(signals);
  }
  bool dnd() {
    std::lock_guard lock(mutex_);
    return dnd_;
  }
  void set_dnd(bool enabled) {
    std::lock_guard lock(mutex_);
    if (dnd_ == enabled) return;
    dnd_ = enabled;
    if (enabled) hide_popups_locked();
    changed_locked();
  }

 private:
  struct Cache {
    std::uint64_t generation = ~0ULL;
    std::vector<Notification> items;
  };

  // --- Model, all under mutex_ ---

  Record* find(std::uint32_t id) {
    const auto found = std::ranges::find(records_, id, &Notification::id);
    return found == records_.end() ? nullptr : &*found;
  }
  void changed_locked() {
    ++generation_;
    const std::uint64_t one = 1;
    const auto written = write(event_, &one, sizeof(one));
    (void)written;
  }
  void hide_popups_locked() {
    bool any = false;
    for (Record& record : records_) any = std::exchange(record.popup, false) || any;
    if (any) changed_locked();
  }
  // Queues NotificationClosed; expiry keeps a record in the history.
  void close_locked(Record& record, std::uint32_t reason, std::vector<Signal>& signals) {
    if (record.closed) return;
    record.closed = true;
    record.close_reason = reason;
    record.popup = false;
    if (reason != expired) record.kept = false;
    signals.push_back({"NotificationClosed", record.id, reason, {}});
  }
  // Closed records outside the history are gone, and the history is capped.
  void prune_locked() {
    std::size_t kept = std::ranges::count_if(records_, [](const Record& r) { return r.kept; });
    for (auto record = records_.begin(); kept > history_limit && record != records_.end(); ++record)
      if (record->kept) {
        record->kept = false;
        --kept;
      }
    std::erase_if(records_, [](const Record& record) {
      if (!record.closed || record.kept) return false;
      remove_file(record.owned_image);
      return true;
    });
  }

  // --- D-Bus thread ---

  void emit(const std::vector<Signal>& signals) {
    if (signals.empty()) return;
    GDBusConnection* connection = nullptr;
    {
      std::lock_guard lock(mutex_);
      if (connection_ != nullptr) connection = G_DBUS_CONNECTION(g_object_ref(connection_));
    }
    if (connection == nullptr) return;
    for (const Signal& signal : signals) {
      GVariant* body = signal.key.empty() && std::string_view(signal.name) == "NotificationClosed"
                           ? g_variant_new("(uu)", signal.id, signal.reason)
                           : g_variant_new("(us)", signal.id, signal.key.c_str());
      g_dbus_connection_emit_signal(connection, nullptr, object_path, bus_name, signal.name, body, nullptr);
    }
    g_object_unref(connection);
  }

  std::uint32_t notify(GVariant* parameters) {
    const gchar *app = nullptr, *icon = nullptr, *summary = nullptr, *body = nullptr;
    guint replaces = 0;
    gint timeout = -1;
    GVariant *actions_raw = nullptr, *hints_raw = nullptr;
    g_variant_get(parameters, "(&su&s&s&s@as@a{sv}i)", &app, &replaces, &icon, &summary, &body, &actions_raw,
                  &hints_raw, &timeout);
    Variant actions = adopt(actions_raw);
    Variant hints_value = adopt(hints_raw);
    const Hints hints(hints_value.get());

    Record record;
    record.app_name = app;
    record.app_icon = icon;
    record.summary = summary;
    record.body = body;
    record.image_path = hints.string({"image-path", "image_path"});
    record.desktop_entry = hints.string({"desktop-entry"});
    record.category = hints.string({"category"});
    record.tag = hints.string({"x-canonical-private-synchronous", "x-dunst-stack-tag", "synchronous", "private-synchronous"});
    record.resident = hints.boolean("resident");
    record.transient = hints.boolean("transient");
    record.action_icons = hints.boolean("action-icons");
    if (const auto urgency = hints.integer("urgency")) record.urgency = static_cast<std::uint8_t>(std::clamp<std::int64_t>(*urgency, 0, 2));
    if (const auto value = hints.integer("value")) record.value = static_cast<int>(std::clamp<std::int64_t>(*value, 0, 100));

    record.icon_source = icon_source(record.app_icon);
    if (record.icon_source.empty()) record.icon_source = desktop_icon(record.desktop_entry);
    for (const char* key : {"image-data", "image_data", "icon_data"})
      if (record.owned_image.empty())
        if (auto data = hints.get(key)) record.owned_image = write_image(data.get());
    record.image_source = record.owned_image.empty() ? icon_source(record.image_path) : record.owned_image;

    const gsize count = g_variant_n_children(actions.get());
    if (count % 2 != 0) {
      remove_file(record.owned_image);
      throw std::invalid_argument("notification actions must be key/label pairs");
    }
    for (gsize index = 0; index < count; index += 2) {
      Variant key = adopt(g_variant_get_child_value(actions.get(), index));
      Variant label = adopt(g_variant_get_child_value(actions.get(), index + 1));
      record.actions.push_back({g_variant_get_string(key.get(), nullptr), g_variant_get_string(label.get(), nullptr)});
    }

    record.expire_timeout = timeout < 0 ? (record.urgency == 2 ? 0 : default_timeout_ms) : timeout;
    record.expires = Clock::now() + std::chrono::milliseconds(record.expire_timeout);
    record.timestamp =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    record.kept = !record.transient;

    std::vector<Signal> signals;
    std::uint32_t id = 0;
    {
      std::lock_guard lock(mutex_);
      record.popup = !dnd_ || record.urgency == 2;
      // What this one replaces: replaces_id, else an open one with its tag.
      Record* previous = replaces != 0 ? find(replaces) : nullptr;
      if (previous == nullptr && !record.tag.empty())
        for (Record& candidate : records_)
          if (!candidate.closed && candidate.tag == record.tag && candidate.app_name == record.app_name)
            previous = &candidate;
      if (previous != nullptr) {
        record.id = previous->id;
        remove_file(previous->owned_image);
        std::erase_if(records_, [id = record.id](const Record& r) { return r.id == id; });
      } else if (replaces != 0) {
        // The spec returns replaces_id even when it names nothing (any more).
        record.id = replaces;
      } else {
        do record.id = next_id_++;
        while (record.id == 0 || find(record.id) != nullptr);
      }
      id = record.id;
      records_.push_back(std::move(record));
      // Too many open: the oldest goes.
      if (static_cast<std::size_t>(std::ranges::count_if(records_, [](const Record& r) { return !r.closed; })) > open_limit)
        for (Record& oldest : records_)
          if (!oldest.closed) {
            close_locked(oldest, evicted, signals);
            break;
          }
      prune_locked();
      changed_locked();
    }
    emit(signals);
    reset_expiry();
    return id;
  }

  // One timer for the earliest deadline among open records.
  void reset_expiry() {
    if (expiry_ != nullptr) {
      g_source_destroy(expiry_);
      g_source_unref(expiry_);
      expiry_ = nullptr;
    }
    auto next = Clock::time_point::max();
    {
      std::lock_guard lock(mutex_);
      for (const Record& record : records_)
        if (!record.closed && record.expire_timeout > 0) next = std::min(next, record.expires);
    }
    if (next == Clock::time_point::max()) return;
    const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(next - Clock::now()).count();
    expiry_ = g_timeout_source_new(static_cast<guint>(std::clamp<std::int64_t>(delay + 1, 1, G_MAXUINT)));
    g_source_set_callback(expiry_, [](gpointer data) -> gboolean {
      auto* self = static_cast<Service*>(data);
      std::vector<Signal> signals;
      {
        std::lock_guard lock(self->mutex_);
        const auto now = Clock::now();
        for (Record& record : self->records_)
          if (!record.closed && record.expire_timeout > 0 && record.expires <= now)
            self->close_locked(record, expired, signals);
        if (!signals.empty()) {
          self->prune_locked();
          self->changed_locked();
        }
      }
      self->emit(signals);
      self->reset_expiry();
      return G_SOURCE_REMOVE;
    }, this, nullptr);
    g_source_attach(expiry_, context_);
  }

  void close_by_sender(std::uint32_t id) {
    std::vector<Signal> signals;
    {
      std::lock_guard lock(mutex_);
      if (Record* record = find(id); record != nullptr && !record->closed) {
        close_locked(*record, withdrawn, signals);
        prune_locked();
        changed_locked();
      }
    }
    emit(signals);
  }

  static void method_call(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method,
                          GVariant* parameters, GDBusMethodInvocation* invocation, gpointer data) {
    auto* self = static_cast<Service*>(data);
    const std::string_view name(method);
    try {
      if (name == "GetCapabilities") {
        const char* capabilities[] = {"actions", "body", "body-markup", "icon-static", "persistence", nullptr};
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(^as)", capabilities));
      } else if (name == "GetServerInformation") {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(ssss)", "vw", "vw", VW_VERSION, "1.2"));
      } else if (name == "Notify") {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", self->notify(parameters)));
      } else if (name == "CloseNotification") {
        guint id = 0;
        g_variant_get(parameters, "(u)", &id);
        self->close_by_sender(id);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
      } else {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod",
                                                   "unknown notification method");
      }
    } catch (const std::exception& error) {
      g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.InvalidArgs", error.what());
    }
  }

  static void connection_closed(GDBusConnection*, gboolean, GError* error, gpointer data) {
    auto* self = static_cast<Service*>(data);
    {
      std::lock_guard lock(self->mutex_);
      self->running_ = false;
      self->error_ = error == nullptr ? "notification D-Bus connection closed" : error->message;
      self->changed_locked();
    }
    if (self->loop_ != nullptr) g_main_loop_quit(self->loop_);
  }

  static std::string take_error(GError* error, const char* fallback) {
    const std::string message = error == nullptr ? fallback : error->message;
    if (error != nullptr) g_error_free(error);
    return message;
  }

  // A private connection, so closing it drops the name with nothing else.
  void run(const std::stop_token& stop) {
    g_main_context_push_thread_default(context_);
    loop_ = g_main_loop_new(context_, FALSE);
    GDBusConnection* connection = nullptr;
    GDBusNodeInfo* info = nullptr;
    guint registration = 0;
    gulong closed_handler = 0;
    bool owned = false;
    try {
      GError* error = nullptr;
      gchar* address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error);
      if (address == nullptr) throw std::runtime_error(take_error(error, "session D-Bus is unavailable"));
      connection = g_dbus_connection_new_for_address_sync(
          address,
          static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
          nullptr, nullptr, &error);
      g_free(address);
      if (connection == nullptr) throw std::runtime_error(take_error(error, "could not connect to session D-Bus"));
      g_dbus_connection_set_exit_on_close(connection, FALSE);
      info = g_dbus_node_info_new_for_xml(interface_xml, nullptr);
      const GDBusInterfaceVTable vtable{method_call, nullptr, nullptr, {nullptr}};
      registration = g_dbus_connection_register_object(connection, object_path, info->interfaces[0], &vtable, this,
                                                       nullptr, &error);
      if (registration == 0) throw std::runtime_error(take_error(error, "could not export notification object"));
      // DO_NOT_QUEUE: fail at once when another daemon has the name.
      Variant reply = adopt(g_dbus_connection_call_sync(
          connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
          g_variant_new("(su)", bus_name, 4U), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error));
      if (!reply) throw std::runtime_error(take_error(error, "could not acquire notification name"));
      guint result = 0;
      g_variant_get(reply.get(), "(u)", &result);
      if (result != 1 && result != 4)
        throw std::runtime_error("another notification daemon already owns org.freedesktop.Notifications");
      owned = true;
      closed_handler = g_signal_connect(connection, "closed", G_CALLBACK(connection_closed), this);
      {
        std::lock_guard lock(mutex_);
        connection_ = connection;
        running_ = true;
        ready_ = true;
        changed_locked();
      }
      ready_condition_.notify_all();
      if (!stop.stop_requested()) g_main_loop_run(loop_);
    } catch (const std::exception& error) {
      std::lock_guard lock(mutex_);
      error_ = error.what();
    }
    if (expiry_ != nullptr) {
      g_source_destroy(expiry_);
      g_source_unref(expiry_);
      expiry_ = nullptr;
    }
    {
      std::lock_guard lock(mutex_);
      connection_ = nullptr;
      running_ = false;
    }
    if (connection != nullptr) {
      if (closed_handler != 0) g_signal_handler_disconnect(connection, closed_handler);
      if (owned)
        Variant released = adopt(g_dbus_connection_call_sync(
            connection, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName",
            g_variant_new("(s)", bus_name), nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr));
      if (registration != 0) g_dbus_connection_unregister_object(connection, registration);
      (void)g_dbus_connection_close_sync(connection, nullptr, nullptr);
      g_object_unref(connection);
    }
    if (info != nullptr) g_dbus_node_info_unref(info);
    g_main_loop_unref(loop_);
    loop_ = nullptr;
    g_main_context_pop_thread_default(context_);
    {
      std::lock_guard lock(mutex_);
      ready_ = true;
      changed_locked();
    }
    ready_condition_.notify_all();
  }

  const int event_;
  GMainContext* const context_;
  GMainLoop* loop_ = nullptr;
  GSource* expiry_ = nullptr;  // D-Bus thread only
  std::jthread thread_;

  std::mutex mutex_;
  std::condition_variable ready_condition_;
  GDBusConnection* connection_ = nullptr;
  std::vector<Record> records_;  // oldest first
  std::uint32_t next_id_ = 1;
  std::uint64_t generation_ = 0;
  std::string error_;
  bool started_ = false;
  bool ready_ = false;
  bool running_ = false;
  bool dnd_ = false;

  // Reader side.
  std::mutex cache_mutex_;
  std::array<Cache, 3> caches_;
  std::unordered_map<std::string, std::string> resolved_;
};

Service& service() {
  static Service instance;
  return instance;
}

}  // namespace

StartResult start() { return service().start(); }
bool running() { return service().running(); }
std::vector<Notification> list() { return service().snapshot(View::open); }
std::vector<Notification> popups() { return service().snapshot(View::popups); }
std::vector<Notification> history() { return service().snapshot(View::history); }
std::uint64_t generation() { return service().generation(); }
bool dismiss(std::uint32_t id) { return service().dismiss(id); }
void hide_popup(std::uint32_t id) { service().hide_popup(id); }
void hide_popups() { service().hide_popups(); }
bool invoke(std::uint32_t id, const std::string& action_key) { return service().invoke(id, action_key); }
bool activate(std::uint32_t id) { return service().activate(id); }
void clear() { service().clear(); }
bool do_not_disturb() { return service().dnd(); }
void set_do_not_disturb(bool enabled) { service().set_dnd(enabled); }
int event_fd() { return service().fd(); }
bool drain_events() { return service().drain(); }

}  // namespace vw::notifications
