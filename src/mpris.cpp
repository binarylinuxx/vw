#include "vw/mpris.hpp"

#include "dbus.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace vw::mpris {
namespace {

constexpr const char* prefix = "org.mpris.MediaPlayer2.";
constexpr const char* path = "/org/mpris/MediaPlayer2";
constexpr const char* player_interface = "org.mpris.MediaPlayer2.Player";
constexpr const char* properties_interface = "org.freedesktop.DBus.Properties";
using Variant = std::unique_ptr<GVariant, decltype(&g_variant_unref)>;


struct Snapshot {
  std::vector<Player> players;
  std::string active_service;
  std::chrono::steady_clock::time_point fetched{};
};

Variant call(GDBusConnection* bus, const char* service, const char* object, const char* interface,
             const char* method, GVariant* arguments, const GVariantType* result = nullptr) {
  if (bus == nullptr) {
    if (arguments != nullptr) g_variant_unref(g_variant_ref_sink(arguments));
    return {nullptr, &g_variant_unref};
  }
  GError* error = nullptr;
  Variant reply(g_dbus_connection_call_sync(
      bus, service, object, interface, method, arguments, result,
      G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error), &g_variant_unref);
  if (error != nullptr) g_error_free(error);
  return reply;
}

Variant property(GVariant* dictionary, const char* key) {
  if (dictionary == nullptr) return {nullptr, &g_variant_unref};
  Variant value(g_variant_lookup_value(dictionary, key, nullptr), &g_variant_unref);
  if (value && g_variant_is_of_type(value.get(), G_VARIANT_TYPE_VARIANT))
    return {g_variant_get_variant(value.get()), &g_variant_unref};
  return value;
}

std::string string_property(GVariant* dictionary, const char* key) {
  auto value = property(dictionary, key);
  if (value && (g_variant_is_of_type(value.get(), G_VARIANT_TYPE_STRING) ||
                g_variant_is_of_type(value.get(), G_VARIANT_TYPE_OBJECT_PATH)))
    return g_variant_get_string(value.get(), nullptr);
  return {};
}

bool bool_property(GVariant* dictionary, const char* key) {
  auto value = property(dictionary, key);
  return value && g_variant_is_of_type(value.get(), G_VARIANT_TYPE_BOOLEAN)
      && g_variant_get_boolean(value.get());
}

double time_property(GVariant* dictionary, const char* key) {
  auto value = property(dictionary, key);
  if (value && g_variant_is_of_type(value.get(), G_VARIANT_TYPE_INT64))
    return static_cast<double>(g_variant_get_int64(value.get())) / 1000000.0;
  return 0;
}

std::string artwork_url(std::string raw) {
  if (raw.empty()) return raw;
  char* scheme = g_uri_parse_scheme(raw.c_str());
  if (scheme != nullptr) {
    g_free(scheme);
    return raw;
  }
  if (g_path_is_absolute(raw.c_str())) {
    char* uri = g_filename_to_uri(raw.c_str(), nullptr, nullptr);
    if (uri != nullptr) {
      raw = uri;
      g_free(uri);
    }
  }
  return raw;
}

Player fetch_player(GDBusConnection* bus, const std::string& service) {
  Player player;
  player.service = service;
  player.name = service.substr(std::char_traits<char>::length(prefix));
  auto reply = call(bus, service.c_str(), path, properties_interface, "GetAll",
                    g_variant_new("(s)", player_interface), G_VARIANT_TYPE("(a{sv})"));
  if (!reply) return player;
  Variant properties(g_variant_get_child_value(reply.get(), 0), &g_variant_unref);
  player.playback_status = string_property(properties.get(), "PlaybackStatus");
  if (player.playback_status.empty()) player.playback_status = "Stopped";
  player.position = time_property(properties.get(), "Position");
  player.can_go_previous = bool_property(properties.get(), "CanGoPrevious");
  player.can_play = bool_property(properties.get(), "CanPlay");
  player.can_pause = bool_property(properties.get(), "CanPause");
  player.can_go_next = bool_property(properties.get(), "CanGoNext");

  auto metadata = property(properties.get(), "Metadata");
  player.title = string_property(metadata.get(), "xesam:title");
  auto artists = property(metadata.get(), "xesam:artist");
  if (artists && g_variant_is_of_type(artists.get(), G_VARIANT_TYPE("as"))) {
    gsize count = 0;
    const gchar** names = g_variant_get_strv(artists.get(), &count);
    for (gsize index = 0; index < count; ++index) {
      if (!player.artist.empty()) player.artist += ", ";
      player.artist += names[index];
    }
    g_free(names);
  }
  player.album = string_property(metadata.get(), "xesam:album");
  player.art_url = artwork_url(string_property(metadata.get(), "mpris:artUrl"));
  for (const char* key : {"xesam:lyrics", "xesam:lyric", "xesam:asText"}) {
    if (player.lyrics.empty()) player.lyrics = string_property(metadata.get(), key);
  }
  player.track_id = string_property(metadata.get(), "mpris:trackid");
  player.length = time_property(metadata.get(), "mpris:length");
  return player;
}

Snapshot fetch_snapshot(GDBusConnection* bus, const std::string& previous) {
  Snapshot result;
  auto reply = call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                    "org.freedesktop.DBus", "ListNames", nullptr,
                    G_VARIANT_TYPE("(as)"));
  if (reply) {
    Variant names(g_variant_get_child_value(reply.get(), 0), &g_variant_unref);
    GVariantIter iter;
    const char* name = nullptr;
    g_variant_iter_init(&iter, names.get());
    while (g_variant_iter_next(&iter, "&s", &name)) {
      if (g_str_has_prefix(name, prefix)) result.players.push_back(fetch_player(bus, name));
    }
  }
  std::ranges::sort(result.players, {}, &Player::service);
  std::string playing;
  for (const auto& player : result.players) {
    if (player.playback_status != "Playing") continue;
    if (!player.art_url.empty()) {
      result.active_service = player.service;
      break;
    }
    if (playing.empty()) playing = player.service;
  }
  if (result.active_service.empty() && !playing.empty()) {
    const auto selected = std::ranges::find(result.players, playing, &Player::service);
    if (selected != result.players.end() && !selected->title.empty()) {
      for (const auto& player : result.players) {
        if (player.title == selected->title && !player.art_url.empty()) {
          result.active_service = player.service;
          break;
        }
      }
    }
  }
  if (result.active_service.empty()) {
    if (!playing.empty()) result.active_service = playing;
    else if (std::ranges::find(result.players, previous, &Player::service) != result.players.end())
      result.active_service = previous;
    else if (!result.players.empty()) result.active_service = result.players.front().service;
  }
  result.fetched = std::chrono::steady_clock::now();
  return result;
}

// Player state lives on a dbus::Monitor thread and is refetched only when a
// player signals (PropertiesChanged, Seeked) or a player name comes or goes;
// position in between is extrapolated. Readers never touch the bus.
class MprisService {
 public:
  MprisService()
      : monitor_(G_BUS_TYPE_SESSION, [this](GDBusConnection* bus) { refresh(bus); },
                 [this](GDBusConnection* bus) { subscribe(bus); }) {}

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
    std::string previous;
    {
      std::lock_guard lock(mutex_);
      previous = snapshot_.active_service;
    }
    Snapshot next = fetch_snapshot(bus, previous);
    std::lock_guard lock(mutex_);
    snapshot_ = std::move(next);
  }

  // Player names are dynamic, so match by path and interface rather than by
  // sender (dbus::Monitor::watch only takes fixed names).
  void subscribe(GDBusConnection* bus) {
    const auto on_signal = [](GDBusConnection*, const gchar*, const gchar*, const gchar*,
                              const gchar*, GVariant*, gpointer data) {
      static_cast<MprisService*>(data)->monitor_.schedule_refresh();
    };
    g_dbus_connection_signal_subscribe(bus, nullptr, properties_interface, "PropertiesChanged", path,
                                       player_interface, G_DBUS_SIGNAL_FLAGS_NONE, on_signal, this,
                                       nullptr);
    g_dbus_connection_signal_subscribe(bus, nullptr, player_interface, "Seeked", path, nullptr,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_signal, this, nullptr);
    g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus", "org.freedesktop.DBus",
                                       "NameOwnerChanged", "/org/freedesktop/DBus", "org.mpris.MediaPlayer2",
                                       G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_NAMESPACE, on_signal, this, nullptr);
  }

  std::mutex mutex_;
  Snapshot snapshot_;
  dbus::Monitor monitor_;
};

MprisService& service() {
  static MprisService instance;
  return instance;
}

const Player* active_player(const Snapshot& snapshot) {
  const auto found = std::ranges::find(snapshot.players, snapshot.active_service,
                                       &Player::service);
  return found == snapshot.players.end() ? nullptr : &*found;
}

double position_now(const Snapshot& snapshot, const Player& player) {
  double position = player.position;
  if (player.playback_status == "Playing")
    position += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - snapshot.fetched).count();
  return player.length > 0 ? std::clamp(position, 0.0, player.length)
                           : std::max(0.0, position);
}

Player current(const Snapshot& snapshot, Player player) {
  player.position = position_now(snapshot, player);
  return player;
}

bool control(const char* method, bool (*allowed)(const Player&)) {
  const auto snapshot = service().snapshot();
  const Player* player = active_player(snapshot);
  if (player == nullptr || !allowed(*player)) return false;
  const bool success = static_cast<bool>(call(service().monitor().connection(), player->service.c_str(),
                                              path, player_interface, method, nullptr));
  service().monitor().schedule_refresh();
  return success;
}

}  // namespace

std::optional<Player> active() {
  const Snapshot snapshot = service().snapshot();
  const Player* player = active_player(snapshot);
  if (player == nullptr) return std::nullopt;
  return current(snapshot, *player);
}

std::vector<Player> players() {
  const Snapshot snapshot = service().snapshot();
  std::vector<Player> result;
  result.reserve(snapshot.players.size());
  for (const Player& player : snapshot.players) result.push_back(current(snapshot, player));
  return result;
}

bool available() { return active_player(service().snapshot()) != nullptr; }

bool previous() {
  return control("Previous", [](const Player& player) { return player.can_go_previous; });
}

bool play_pause() {
  return control("PlayPause", [](const Player& player) { return player.can_play || player.can_pause; });
}

bool next() {
  return control("Next", [](const Player& player) { return player.can_go_next; });
}

bool set_position(double seconds) {
  if (!std::isfinite(seconds)) throw std::invalid_argument("position must be finite");
  const auto snapshot = service().snapshot();
  const Player* player = active_player(snapshot);
  if (player == nullptr || player->length <= 0 || !g_variant_is_object_path(player->track_id.c_str()))
    return false;
  const double clamped = std::clamp(seconds, 0.0, player->length);
  const bool success = static_cast<bool>(call(service().monitor().connection(), player->service.c_str(),
      path, player_interface, "SetPosition", g_variant_new("(ox)", player->track_id.c_str(),
                                    static_cast<std::int64_t>(clamped * 1000000.0))));
  service().monitor().schedule_refresh();
  return success;
}

int event_fd() { return service().monitor().fd(); }
bool drain_events() { return service().monitor().drain(); }

}  // namespace vw::mpris
