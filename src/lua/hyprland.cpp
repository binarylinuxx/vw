#include "vw/hyprland.hpp"
#include "vw/lua/modules.hpp"
#include "vw/lua_util.hpp"

#include <array>
#include <string>
#include <utility>

namespace vw {
namespace {

int query(lua_State* lua) {
  push_json(lua, hyprland::query(lua_tostring(lua, lua_upvalueindex(1))));
  return 1;
}

int generic_query(lua_State* lua) {
  push_json(lua, hyprland::query(luaL_checkstring(lua, 1)));
  return 1;
}

int raw_request(lua_State* lua) {
  const std::string response = hyprland::request(luaL_checkstring(lua, 1));
  lua_pushlstring(lua, response.data(), response.size());
  return 1;
}

// dispatch("workspace 2") / keyword("general:gaps_in 4") -> ok, response
int command(lua_State* lua) {
  const std::string_view verb = lua_tostring(lua, lua_upvalueindex(1));
  const char* arguments = luaL_checkstring(lua, 1);
  const auto result = verb == "dispatch" ? hyprland::dispatch(arguments)
                                         : hyprland::keyword(arguments);
  lua_pushboolean(lua, result.ok);
  lua_pushlstring(lua, result.response.data(), result.response.size());
  return 2;
}

int last_event(lua_State* lua) {
  const auto event = hyprland::last_event();
  if (!event) {
    lua_pushnil(lua);
    return 1;
  }
  lua_createtable(lua, 0, 3);
  lua_pushlstring(lua, event->name.data(), event->name.size());
  lua_setfield(lua, -2, "name");
  lua_pushlstring(lua, event->data.data(), event->data.size());
  lua_setfield(lua, -2, "data");
  lua_pushinteger(lua, static_cast<lua_Integer>(event->sequence));
  lua_setfield(lua, -2, "sequence");
  return 1;
}

int available(lua_State* lua) {
  lua_pushboolean(lua, hyprland::available());
  return 1;
}

}  // namespace

void install_hyprland(lua_State* lua) {
  lua_newtable(lua);
  const std::array<std::pair<const char*, const char*>, 14> queries{{
      {"workspaces", "workspaces"}, {"monitors", "monitors"}, {"clients", "clients"},
      {"active_workspace", "activeworkspace"}, {"active_window", "activewindow"},
      {"activeworkspace", "activeworkspace"}, {"activewindow", "activewindow"},
      {"devices", "devices"}, {"version", "version"}, {"layers", "layers"},
      {"binds", "binds"}, {"cursor_position", "cursorpos"},
      {"workspace_rules", "workspacerules"}, {"layouts", "layouts"}}};
  for (const auto& [name, endpoint] : queries) {
    lua_pushstring(lua, endpoint);
    lua_pushcclosure(lua, guarded<query>, 1);
    lua_setfield(lua, -2, name);
  }
  for (const char* verb : {"dispatch", "keyword"}) {
    lua_pushstring(lua, verb);
    lua_pushcclosure(lua, guarded<command>, 1);
    lua_setfield(lua, -2, verb);
  }
  set_function(lua, "query", guarded<generic_query>);
  set_function(lua, "request", guarded<raw_request>);
  set_function(lua, "available", available);
  set_function(lua, "last_event", last_event);
}

}  // namespace vw
