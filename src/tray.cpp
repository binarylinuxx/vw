#include "vw/tray.hpp"

#include "dbus.hpp"

#include <cairo.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <unistd.h>

namespace vw::tray {
namespace {

namespace fs = std::filesystem;
constexpr const char* watcher_name = "org.kde.StatusNotifierWatcher";
constexpr const char* watcher_path = "/StatusNotifierWatcher";
constexpr const char* item_interface = "org.kde.StatusNotifierItem";
constexpr const char* menu_interface = "com.canonical.dbusmenu";
constexpr const char* watcher_xml = R"XML(
<node><interface name="org.kde.StatusNotifierWatcher">
  <method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
  <method name="RegisterStatusNotifierHost"><arg type="s" direction="in"/></method>
  <property name="RegisteredStatusNotifierItems" type="as" access="read"/>
  <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
  <property name="ProtocolVersion" type="i" access="read"/>
  <signal name="StatusNotifierItemRegistered"><arg type="s"/></signal>
  <signal name="StatusNotifierItemUnregistered"><arg type="s"/></signal>
  <signal name="StatusNotifierHostRegistered"/>
  <signal name="StatusNotifierHostUnregistered"/>
</interface></node>)XML";

struct Entry {
  std::string bus;
  std::string path;
  Item item;
  std::string menu_path;
  std::optional<std::vector<MenuEntry>> menu;
  // Monitor-thread bookkeeping.
  guint name_watch = 0;
  guint item_signals = 0;
  guint menu_signals = 0;
};

// "bus/path", "/path" (bus is the sender) or a bare bus name.
std::pair<std::string, std::string> parse_service(const std::string& service,
                                                  const std::string& sender) {
  if (service.starts_with('/')) return {sender, service};
  const auto slash = service.find('/');
  if (slash == std::string::npos) return {service, "/StatusNotifierItem"};
  return {service.substr(0, slash), service.substr(slash)};
}

// user_data for per-item signal subscriptions.
struct Keyed {
  class Tray* self;
  std::string key;
};
void free_keyed(gpointer data) { delete static_cast<Keyed*>(data); }

std::string strip_mnemonics(const std::string& label) {
  std::string result;
  for (std::size_t index = 0; index < label.size(); ++index) {
    if (label[index] == '_') {
      if (index + 1 < label.size() && label[index + 1] == '_') result += '_', ++index;
      continue;
    }
    result += label[index];
  }
  return result;
}

fs::path cache_directory() {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  return fs::path(runtime != nullptr && *runtime != '\0' ? runtime : "/tmp") / "vw-tray";
}

// Writes the largest ARGB32 pixmap in an a(iiay) as a PNG named by its
// content; returns the path, or empty when there is none.
std::string pixmap_file(GVariant* pixmaps) {
  if (pixmaps == nullptr || !g_variant_is_of_type(pixmaps, G_VARIANT_TYPE("a(iiay)"))) return {};
  dbus::Variant best = dbus::adopt(nullptr);
  gint32 best_width = 0;
  gint32 best_height = 0;
  const gsize count = g_variant_n_children(pixmaps);
  for (gsize index = 0; index < count; ++index) {
    auto pixmap = dbus::adopt(g_variant_get_child_value(pixmaps, index));
    gint32 width = 0;
    gint32 height = 0;
    g_variant_get_child(pixmap.get(), 0, "i", &width);
    g_variant_get_child(pixmap.get(), 1, "i", &height);
    if (width > 0 && height > 0 && width * height > best_width * best_height) {
      best = dbus::adopt(g_variant_get_child_value(pixmap.get(), 2));
      best_width = width;
      best_height = height;
    }
  }
  if (!best) return {};
  gsize size = 0;
  const auto* bytes = static_cast<const std::uint8_t*>(
      g_variant_get_fixed_array(best.get(), &size, 1));
  if (size < static_cast<gsize>(best_width) * best_height * 4) return {};

  std::uint64_t hash = 1469598103934665603ULL;  // FNV-1a
  for (gsize index = 0; index < size; ++index) hash = (hash ^ bytes[index]) * 1099511628211ULL;
  const fs::path directory = cache_directory();
  const fs::path file = directory / (std::to_string(best_width) + "x" + std::to_string(best_height) +
                                     "-" + std::to_string(hash) + ".png");
  std::error_code error;
  if (fs::exists(file, error)) return file.string();
  fs::create_directories(directory, error);

  // Network byte order, straight alpha → native-endian premultiplied.
  cairo_surface_t* surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, best_width, best_height);
  auto* pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  for (gint32 y = 0; y < best_height; ++y) {
    auto* row = reinterpret_cast<std::uint32_t*>(pixels + static_cast<std::ptrdiff_t>(y) * stride);
    for (gint32 x = 0; x < best_width; ++x) {
      const std::uint8_t* source = bytes + (static_cast<gsize>(y) * best_width + x) * 4;
      const std::uint32_t alpha = source[0];
      const auto premultiply = [alpha](std::uint32_t channel) { return channel * alpha / 255; };
      row[x] = alpha << 24 | premultiply(source[1]) << 16 | premultiply(source[2]) << 8 |
               premultiply(source[3]);
    }
  }
  cairo_surface_mark_dirty(surface);
  const fs::path temporary = file.string() + ".tmp" + std::to_string(getpid());
  const bool written = cairo_surface_write_to_png(surface, temporary.c_str()) == CAIRO_STATUS_SUCCESS;
  cairo_surface_destroy(surface);
  if (!written) return {};
  fs::rename(temporary, file, error);
  return error ? std::string() : file.string();
}

// An icon name looked up in an item's private theme directory.
std::string theme_file(const std::string& directory, const std::string& name) {
  if (directory.empty() || name.empty()) return {};
  std::error_code error;
  std::string fallback;
  for (auto it = fs::recursive_directory_iterator(directory, error);
       !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
    const fs::path& file = it->path();
    if (file.stem() != name) continue;
    const auto extension = file.extension();
    if (extension == ".svg") return file.string();
    if (extension == ".png" && fallback.empty()) fallback = file.string();
  }
  return fallback;
}

MenuEntry parse_menu(GVariant* node) {
  MenuEntry entry;
  gint32 id = 0;
  GVariant* properties = nullptr;
  GVariant* children = nullptr;
  g_variant_get(node, "(i@a{sv}@av)", &id, &properties, &children);
  auto owned_properties = dbus::adopt(properties);
  auto owned_children = dbus::adopt(children);
  entry.id = id;
  entry.label = strip_mnemonics(dbus::get_string(properties, "label"));
  entry.icon = dbus::get_string(properties, "icon-name");
  entry.enabled = dbus::get_bool(properties, "enabled", true);
  entry.visible = dbus::get_bool(properties, "visible", true);
  entry.separator = dbus::get_string(properties, "type") == "separator";
  const std::string toggle = dbus::get_string(properties, "toggle-type");
  entry.toggle = toggle == "checkmark" ? MenuEntry::Toggle::checkmark
                 : toggle == "radio"   ? MenuEntry::Toggle::radio
                                       : MenuEntry::Toggle::none;
  entry.checked = dbus::get_int(properties, "toggle-state") == 1;
  const gsize count = g_variant_n_children(children);
  for (gsize index = 0; index < count; ++index) {
    auto child = dbus::adopt(g_variant_get_child_value(children, index));
    auto inner = dbus::adopt(g_variant_get_variant(child.get()));
    if (g_variant_is_of_type(inner.get(), G_VARIANT_TYPE("(ia{sv}av)")))
      entry.children.push_back(parse_menu(inner.get()));
  }
  return entry;
}

class Tray {
 public:
  Tray()
      : monitor_(G_BUS_TYPE_SESSION, [](GDBusConnection*) {},
                 [this](GDBusConnection* bus) { setup(bus); }) {}

  dbus::Monitor& monitor() {
    monitor_.start();
    return monitor_;
  }
  bool available() {
    monitor_.start();
    std::lock_guard lock(mutex_);
    return watcher_present_;
  }
  std::vector<Item> items() {
    monitor_.start();
    std::lock_guard lock(mutex_);
    std::vector<Item> result;
    for (const auto& key : order_) result.push_back(entries_.at(key).item);
    return result;
  }
  // Bus name and path of an item, or nullopt.
  std::optional<std::pair<std::string, std::string>> address(const std::string& id) {
    monitor_.start();
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(id);
    if (found == entries_.end()) return std::nullopt;
    return std::pair{found->second.bus, found->second.path};
  }

  void invalidate_menu(const std::string& id) {
    {
      std::lock_guard lock(mutex_);
      const auto found = entries_.find(id);
      if (found != entries_.end()) found->second.menu.reset();
    }
    monitor_.wake();
  }

  // Bus name and menu object of an item, or nullopt without a menu.
  std::optional<std::pair<std::string, std::string>> menu_address(const std::string& id) {
    monitor_.start();
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(id);
    if (found == entries_.end() || found->second.menu_path.empty()) return std::nullopt;
    return std::pair{found->second.bus, found->second.menu_path};
  }

  std::vector<MenuEntry> menu(const std::string& id) {
    std::string bus;
    std::string path;
    {
      monitor_.start();
      std::lock_guard lock(mutex_);
      const auto found = entries_.find(id);
      if (found == entries_.end() || found->second.menu_path.empty()) return {};
      if (found->second.menu) return *found->second.menu;
      bus = found->second.bus;
      path = found->second.menu_path;
    }
    GDBusConnection* connection = monitor_.connection();
    dbus::call(connection, bus.c_str(), path.c_str(), menu_interface, "AboutToShow",
               g_variant_new("(i)", 0), nullptr, 500);
    auto reply = dbus::call(connection, bus.c_str(), path.c_str(), menu_interface, "GetLayout",
                            g_variant_new("(ii@as)", 0, -1, g_variant_new_strv(nullptr, 0)),
                            "(u(ia{sv}av))", 1000);
    std::vector<MenuEntry> entries;
    if (reply) {
      auto root = dbus::adopt(g_variant_get_child_value(reply.get(), 1));
      entries = parse_menu(root.get()).children;
    }
    {
      std::lock_guard lock(mutex_);
      const auto found = entries_.find(id);
      if (found != entries_.end()) found->second.menu = entries;
    }
    // Invalidate the cache when the menu changes.
    monitor_.invoke([this, id, bus, path] {
      auto found = entries_.find(id);
      if (found == entries_.end() || found->second.menu_signals != 0) return;
      found->second.menu_signals = g_dbus_connection_signal_subscribe(
          monitor_.connection(), bus.c_str(), menu_interface, nullptr, path.c_str(), nullptr,
          G_DBUS_SIGNAL_FLAGS_NONE,
          [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*,
             gpointer data) {
            const auto* keyed = static_cast<Keyed*>(data);
            keyed->self->invalidate_menu(keyed->key);
          },
          new Keyed{this, id}, free_keyed);
    });
    return entries;
  }

 private:
  void setup(GDBusConnection* bus) {
    unique_name_ = g_dbus_connection_get_unique_name(bus);
    GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(watcher_xml, nullptr);
    static const GDBusInterfaceVTable vtable{
        [](GDBusConnection*, const gchar* sender, const gchar*, const gchar*, const gchar* method,
           GVariant* parameters, GDBusMethodInvocation* invocation, gpointer data) {
          auto* self = static_cast<Tray*>(data);
          const gchar* argument = nullptr;
          g_variant_get(parameters, "(&s)", &argument);
          if (std::string_view(method) == "RegisterStatusNotifierItem") {
            const auto [bus_name, path] = parse_service(argument, sender);
            self->add_item(bus_name, path);
          } else {
            self->emit("StatusNotifierHostRegistered", nullptr);
          }
          g_dbus_method_invocation_return_value(invocation, nullptr);
        },
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* property,
           GError**, gpointer data) -> GVariant* {
          auto* self = static_cast<Tray*>(data);
          const std::string_view name(property);
          if (name == "ProtocolVersion") return g_variant_new_int32(0);
          if (name == "IsStatusNotifierHostRegistered") return g_variant_new_boolean(TRUE);
          GVariantBuilder builder;
          g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
          std::lock_guard lock(self->mutex_);
          for (const auto& key : self->order_) g_variant_builder_add(&builder, "s", key.c_str());
          return g_variant_builder_end(&builder);
        },
        nullptr, {}};
    g_dbus_connection_register_object(bus, watcher_path, node->interfaces[0], &vtable, this,
                                      nullptr, nullptr);
    g_dbus_node_info_unref(node);

    // Own the watcher name when free (queued otherwise, so vw takes over when
    // another bar exits), and follow whoever owns it.
    g_bus_own_name_on_connection(
        bus, watcher_name, G_BUS_NAME_OWNER_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, gpointer data) {
          auto* self = static_cast<Tray*>(data);
          self->owns_watcher_ = true;
          self->emit("StatusNotifierHostRegistered", nullptr);
        },
        [](GDBusConnection*, const gchar*, gpointer data) {
          static_cast<Tray*>(data)->owns_watcher_ = false;
        },
        this, nullptr);
    const std::string host = "org.kde.StatusNotifierHost-" + std::to_string(getpid());
    g_bus_own_name_on_connection(bus, host.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE, nullptr, nullptr,
                                 nullptr, nullptr);
    g_bus_watch_name_on_connection(
        bus, watcher_name, G_BUS_NAME_WATCHER_FLAGS_NONE,
        [](GDBusConnection* connection, const gchar*, const gchar* owner, gpointer data) {
          static_cast<Tray*>(data)->watcher_appeared(connection, owner);
        },
        [](GDBusConnection*, const gchar*, gpointer data) {
          static_cast<Tray*>(data)->watcher_vanished();
        },
        this, nullptr);
  }

  void watcher_appeared(GDBusConnection* bus, const std::string& owner) {
    {
      std::lock_guard lock(mutex_);
      watcher_present_ = true;
    }
    monitor_.wake();
    if (owner == unique_name_) return;  // our own watcher: items register with us
    // Another bar's watcher: register as a host and mirror its items.
    const std::string host = "org.kde.StatusNotifierHost-" + std::to_string(getpid());
    g_dbus_connection_call(bus, watcher_name, watcher_path, watcher_name,
                           "RegisterStatusNotifierHost", g_variant_new("(s)", host.c_str()),
                           nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr, nullptr);
    external_signals_ = g_dbus_connection_signal_subscribe(
        bus, watcher_name, watcher_name, nullptr, watcher_path, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* signal,
           GVariant* parameters, gpointer data) {
          auto* self = static_cast<Tray*>(data);
          if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(s)"))) return;
          const gchar* service = nullptr;
          g_variant_get(parameters, "(&s)", &service);
          const auto [bus_name, path] = parse_service(service, "");
          if (bus_name.empty()) return;
          if (std::string_view(signal) == "StatusNotifierItemRegistered")
            self->add_item(bus_name, path);
          else if (std::string_view(signal) == "StatusNotifierItemUnregistered")
            self->remove_item(bus_name + path);
        },
        this, nullptr);
    g_dbus_connection_call(
        bus, watcher_name, watcher_path, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", watcher_name, "RegisteredStatusNotifierItems"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result,
                                                          nullptr);
          if (reply == nullptr) return;
          auto owned = dbus::adopt(reply);
          auto value = dbus::adopt(g_variant_get_child_value(reply, 0));
          auto list = dbus::adopt(g_variant_get_variant(value.get()));
          if (!g_variant_is_of_type(list.get(), G_VARIANT_TYPE("as"))) return;
          GVariantIter iter;
          g_variant_iter_init(&iter, list.get());
          const gchar* service = nullptr;
          while (g_variant_iter_next(&iter, "&s", &service)) {
            const auto [bus_name, path] = parse_service(service, "");
            if (!bus_name.empty()) static_cast<Tray*>(data)->add_item(bus_name, path);
          }
        },
        this);
  }

  void watcher_vanished() {
    GDBusConnection* bus = monitor_.connection();
    if (external_signals_ != 0) {
      g_dbus_connection_signal_unsubscribe(bus, external_signals_);
      external_signals_ = 0;
    }
    // Items re-register with whichever watcher comes next.
    std::vector<std::string> keys;
    {
      std::lock_guard lock(mutex_);
      watcher_present_ = false;
      keys = order_;
    }
    for (const auto& key : keys) remove_item(key);
    monitor_.wake();
  }

  void emit(const char* signal, GVariant* body) {
    if (!owns_watcher_) {
      if (body != nullptr) g_variant_unref(g_variant_ref_sink(body));
      return;
    }
    g_dbus_connection_emit_signal(monitor_.connection(), nullptr, watcher_path, watcher_name,
                                  signal, body, nullptr);
  }

  void add_item(const std::string& bus, const std::string& path) {
    const std::string key = bus + path;
    {
      std::lock_guard lock(mutex_);
      if (entries_.contains(key)) return;
      Entry entry;
      entry.bus = bus;
      entry.path = path;
      entry.item.id = key;
      entries_.emplace(key, std::move(entry));
      order_.push_back(key);
    }
    GDBusConnection* connection = monitor_.connection();
    const guint watch = g_bus_watch_name_on_connection_with_closures(
        connection, bus.c_str(), G_BUS_NAME_WATCHER_FLAGS_NONE, nullptr,
        g_cclosure_new(G_CALLBACK(+[](GDBusConnection*, const gchar*, gpointer data) {
                         const auto* keyed = static_cast<Keyed*>(data);
                         keyed->self->remove_item(keyed->key);
                       }),
                       new Keyed{this, key}, [](gpointer data, GClosure*) { free_keyed(data); }));
    const guint signals = g_dbus_connection_signal_subscribe(
        connection, bus.c_str(), nullptr, nullptr, path.c_str(), nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* signal,
           GVariant*, gpointer data) {
          // NewIcon, NewTitle, NewStatus, NewToolTip, ...
          if (!std::string_view(signal).starts_with("New")) return;
          const auto* keyed = static_cast<Keyed*>(data);
          keyed->self->fetch(keyed->key);
        },
        new Keyed{this, key}, free_keyed);
    {
      std::lock_guard lock(mutex_);
      auto& entry = entries_.at(key);
      entry.name_watch = watch;
      entry.item_signals = signals;
    }
    emit("StatusNotifierItemRegistered", g_variant_new("(s)", key.c_str()));
    fetch(key);
  }

  void remove_item(const std::string& key) {
    Entry entry;
    {
      std::lock_guard lock(mutex_);
      const auto found = entries_.find(key);
      if (found == entries_.end()) return;
      entry = std::move(found->second);
      entries_.erase(found);
      std::erase(order_, key);
    }
    GDBusConnection* connection = monitor_.connection();
    if (entry.name_watch != 0) g_bus_unwatch_name(entry.name_watch);
    if (entry.item_signals != 0) g_dbus_connection_signal_unsubscribe(connection, entry.item_signals);
    if (entry.menu_signals != 0) g_dbus_connection_signal_unsubscribe(connection, entry.menu_signals);
    emit("StatusNotifierItemUnregistered", g_variant_new("(s)", key.c_str()));
    monitor_.wake();
  }

  void fetch(const std::string& key) {
    std::string bus;
    std::string path;
    {
      std::lock_guard lock(mutex_);
      const auto found = entries_.find(key);
      if (found == entries_.end()) return;
      bus = found->second.bus;
      path = found->second.path;
    }
    g_dbus_connection_call(
        monitor_.connection(), bus.c_str(), path.c_str(), "org.freedesktop.DBus.Properties",
        "GetAll", g_variant_new("(s)", item_interface), G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE, 2000, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          std::unique_ptr<Keyed> keyed(static_cast<Keyed*>(data));
          GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result,
                                                          nullptr);
          if (reply == nullptr) return;
          auto owned = dbus::adopt(reply);
          auto properties = dbus::adopt(g_variant_get_child_value(reply, 0));
          keyed->self->update(keyed->key, properties.get());
        },
        new Keyed{this, key});
  }

  void update(const std::string& key, GVariant* properties) {
    Item item;
    item.id = key;
    item.name = dbus::get_string(properties, "Id");
    item.title = dbus::get_string(properties, "Title");
    item.category = dbus::get_string(properties, "Category");
    const std::string status = dbus::get_string(properties, "Status");
    item.status = status == "Passive"          ? Status::passive
                  : status == "NeedsAttention" ? Status::needs_attention
                                               : Status::active;
    item.item_is_menu = dbus::get_bool(properties, "ItemIsMenu");
    const std::string menu_path = dbus::get_string(properties, "Menu");
    item.has_menu = menu_path.size() > 1;

    const bool attention = item.status == Status::needs_attention;
    std::string name = attention ? dbus::get_string(properties, "AttentionIconName") : "";
    auto pixmaps = dbus::lookup(properties, attention ? "AttentionIconPixmap" : "IconPixmap");
    if (attention && name.empty() && !pixmaps) {
      name = dbus::get_string(properties, "IconName");
      pixmaps = dbus::lookup(properties, "IconPixmap");
    } else if (!attention) {
      name = dbus::get_string(properties, "IconName");
    }
    if (name.starts_with('/')) item.icon_path = name;
    else item.icon_path = theme_file(dbus::get_string(properties, "IconThemePath"), name);
    if (item.icon_path.empty() && !name.empty()) item.icon = name;
    if (item.icon_path.empty() && item.icon.empty()) item.icon_path = pixmap_file(pixmaps.get());

    auto tooltip = dbus::lookup(properties, "ToolTip");
    if (tooltip && g_variant_is_of_type(tooltip.get(), G_VARIANT_TYPE("(sa(iiay)ss)"))) {
      const gchar* title = nullptr;
      const gchar* body = nullptr;
      g_variant_get(tooltip.get(), "(&s@a(iiay)&s&s)", nullptr, nullptr, &title, &body);
      item.tooltip_title = title;
      item.tooltip_body = body;
    }
    {
      std::lock_guard lock(mutex_);
      const auto found = entries_.find(key);
      if (found == entries_.end()) return;
      const std::string menu = item.has_menu ? menu_path : "";
      found->second.item = std::move(item);
      if (found->second.menu_path != menu) found->second.menu.reset();
      found->second.menu_path = menu;
    }
    monitor_.wake();
  }

  std::mutex mutex_;
  std::map<std::string, Entry> entries_;
  std::vector<std::string> order_;
  bool watcher_present_ = false;
  // Monitor thread only.
  std::string unique_name_;
  bool owns_watcher_ = false;
  guint external_signals_ = 0;
  dbus::Monitor monitor_;
};

Tray& tray() {
  static Tray instance;
  return instance;
}

// Fire-and-forget: items may block while showing their own UI.
bool item_call(const std::string& id, const char* method, GVariant* arguments) {
  const auto address = tray().address(id);
  if (!address) {
    g_variant_unref(g_variant_ref_sink(arguments));
    return false;
  }
  g_dbus_connection_call(tray().monitor().connection(), address->first.c_str(),
                         address->second.c_str(), item_interface, method, arguments, nullptr,
                         G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr, nullptr);
  return true;
}

}  // namespace

void start() { tray().monitor(); }
bool available() { return tray().available(); }
std::vector<Item> items() { return tray().items(); }

bool activate(const std::string& id, int x, int y) {
  return item_call(id, "Activate", g_variant_new("(ii)", x, y));
}
bool secondary_activate(const std::string& id, int x, int y) {
  return item_call(id, "SecondaryActivate", g_variant_new("(ii)", x, y));
}
bool context_menu(const std::string& id, int x, int y) {
  return item_call(id, "ContextMenu", g_variant_new("(ii)", x, y));
}
bool scroll(const std::string& id, int delta, bool horizontal) {
  return item_call(id, "Scroll", g_variant_new("(is)", delta, horizontal ? "horizontal" : "vertical"));
}

std::vector<MenuEntry> menu(const std::string& id) { return tray().menu(id); }

bool activate_menu(const std::string& id, std::int32_t entry) {
  const auto address = tray().menu_address(id);
  if (!address) return false;
  // Fire-and-forget: the item may open a window before replying. It reports
  // layout changes, which invalidate the cached menu().
  g_dbus_connection_call(tray().monitor().connection(), address->first.c_str(),
                         address->second.c_str(), menu_interface, "Event",
                         g_variant_new("(isvu)", entry, "clicked", g_variant_new_int32(0), 0U),
                         nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr, nullptr);
  return true;
}

int event_fd() { return tray().monitor().fd(); }
bool drain_events() { return tray().monitor().drain(); }

}  // namespace vw::tray
