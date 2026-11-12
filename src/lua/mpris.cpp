#include "vw/lua/modules.hpp"
#include "vw/lua_util.hpp"
#include "vw/mpris.hpp"

#include <optional>
#include <string>

namespace vw {
namespace {

void set_string(lua_State* lua, const char* key, const std::string& value) {
  lua_pushlstring(lua, value.data(), value.size());
  lua_setfield(lua, -2, key);
}

void set_bool(lua_State* lua, const char* key, bool value) {
  lua_pushboolean(lua, value);
  lua_setfield(lua, -2, key);
}

// A missing player pushes the same table shape with available = false.
void push_player(lua_State* lua, const std::optional<mpris::Player>& player) {
  const mpris::Player empty;
  const mpris::Player& value = player ? *player : empty;
  lua_createtable(lua, 0, 18);
  set_bool(lua, "available", player.has_value());
  set_string(lua, "active_player", value.service);
  set_string(lua, "player_name", value.name);
  set_string(lua, "title", value.title);
  set_string(lua, "artist", value.artist);
  set_string(lua, "album", value.album);
  set_string(lua, "art_url", value.art_url);
  set_string(lua, "lyrics", value.lyrics);
  set_string(lua, "playback_status", value.playback_status);
  lua_pushnumber(lua, value.position);
  lua_setfield(lua, -2, "position");
  lua_pushnumber(lua, value.length);
  lua_setfield(lua, -2, "length");
  set_bool(lua, "can_go_previous", value.can_go_previous);
  set_bool(lua, "can_play", value.can_play);
  set_bool(lua, "can_pause", value.can_pause);
  set_bool(lua, "can_go_next", value.can_go_next);
}

int state(lua_State* lua) {
  push_player(lua, mpris::active());
  return 1;
}

int players(lua_State* lua) {
  const auto list = mpris::players();
  lua_createtable(lua, static_cast<int>(list.size()), 0);
  lua_Integer index = 1;
  for (const auto& player : list) {
    push_player(lua, player);
    lua_rawseti(lua, -2, index++);
  }
  return 1;
}

int available(lua_State* lua) {
  lua_pushboolean(lua, mpris::available());
  return 1;
}

int control(lua_State* lua) {
  const lua_Integer method = lua_tointeger(lua, lua_upvalueindex(1));
  lua_pushboolean(lua, method == 0 ? mpris::previous() : method == 1 ? mpris::play_pause()
                                                                     : mpris::next());
  return 1;
}

int set_position(lua_State* lua) {
  lua_pushboolean(lua, mpris::set_position(luaL_checknumber(lua, 1)));
  return 1;
}

}  // namespace

void install_mpris(lua_State* lua) {
  lua_newtable(lua);
  set_function(lua, "state", guarded<state>);
  set_function(lua, "players", guarded<players>);
  set_function(lua, "available", guarded<available>);
  for (lua_Integer method = 0; method < 3; ++method) {
    lua_pushinteger(lua, method);
    lua_pushcclosure(lua, guarded<control>, 1);
    lua_setfield(lua, -2, method == 0 ? "previous" : method == 1 ? "play_pause" : "next");
  }
  set_function(lua, "set_position", guarded<set_position>);
}

}  // namespace vw
