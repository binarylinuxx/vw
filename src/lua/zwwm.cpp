#include "vw/lua/modules.hpp"
#include "vw/lua_util.hpp"
#include "vw/zwwm.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vw {
namespace {

int available(lua_State* lua) {
  lua_pushboolean(lua, zwwm::available());
  return 1;
}

int snapshot(lua_State* lua) {
  push_json(lua, zwwm::snapshot(static_cast<zwwm::Snapshot>(lua_tointeger(lua, lua_upvalueindex(1)))));
  return 1;
}

int action(lua_State* lua) {
  zwwm::Result result;
  switch (lua_tointeger(lua, lua_upvalueindex(1))) {
    case 0: result = zwwm::dispatch(luaL_checkstring(lua, 1), luaL_optstring(lua, 2, "")); break;
    case 1: result = zwwm::reload(); break;
    case 2: result = zwwm::rebuild_switch_shaders(); break;
    default: {
      const char* theme = luaL_checkstring(lua, 1);
      const lua_Integer size = luaL_checkinteger(lua, 2);
      if (size < 0 || size > std::numeric_limits<std::uint32_t>::max())
        throw std::out_of_range("cursor size is out of range");
      result = zwwm::set_cursor(theme, static_cast<std::uint32_t>(size));
    }
  }
  lua_pushboolean(lua, result.success);
  lua_pushlstring(lua, result.message.data(), result.message.size());
  return 2;
}

int tags(lua_State* lua) {
  const auto list = zwwm::tags();
  lua_createtable(lua, static_cast<int>(list.size()), 0);
  lua_Integer index = 1;
  for (const auto& tag : list) {
    lua_createtable(lua, 0, 3);
    lua_pushinteger(lua, tag.id);
    lua_setfield(lua, -2, "id");
    lua_pushlstring(lua, tag.name.data(), tag.name.size());
    lua_setfield(lua, -2, "name");
    lua_pushboolean(lua, tag.active);
    lua_setfield(lua, -2, "active");
    lua_rawseti(lua, -2, index++);
  }
  return 1;
}

int active_tags(lua_State* lua) {
  const auto ids = zwwm::active_tags();
  lua_createtable(lua, static_cast<int>(ids.size()), 0);
  for (std::size_t index = 0; index < ids.size(); ++index) {
    lua_pushinteger(lua, ids[index]);
    lua_rawseti(lua, -2, static_cast<lua_Integer>(index + 1));
  }
  return 1;
}

int set_active_tags(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  const lua_Integer count = luaL_len(lua, 1);
  std::vector<std::uint32_t> ids;
  ids.reserve(static_cast<std::size_t>(std::max<lua_Integer>(count, 0)));
  for (lua_Integer index = 1; index <= count; ++index) {
    lua_geti(lua, 1, index);
    if (!lua_isinteger(lua, -1)) throw std::invalid_argument("tag ids must be integers");
    const lua_Integer id = lua_tointeger(lua, -1);
    lua_pop(lua, 1);
    if (id < 0 || id > std::numeric_limits<std::uint32_t>::max())
      throw std::out_of_range("tag id is out of range");
    ids.push_back(static_cast<std::uint32_t>(id));
  }
  zwwm::set_active_tags(ids);
  lua_pushboolean(lua, 1);
  return 1;
}

int last_event(lua_State* lua) {
  const std::string event = zwwm::last_event();
  lua_pushlstring(lua, event.data(), event.size());
  return 1;
}

}  // namespace

void install_zwwm(lua_State* lua) {
  lua_newtable(lua);
  set_function(lua, "available", available);
  set_function(lua, "tags", guarded<tags>);
  set_function(lua, "active_tags", guarded<active_tags>);
  set_function(lua, "set_active_tags", guarded<set_active_tags>);
  set_function(lua, "last_event", guarded<last_event>);
  for (const auto& [name, type] : {
           std::pair{"status", 0}, {"outputs", 1}, {"toplevels", 2},
           {"output_tags", 3}, {"layers", 4}, {"cameras", 5},
           {"keyboard", 6}, {"live", 7}}) {
    lua_pushinteger(lua, type);
    lua_pushcclosure(lua, guarded<snapshot>, 1);
    lua_setfield(lua, -2, name);
  }
  for (const auto& [name, type] : {
           std::pair{"dispatch", 0}, {"reload", 1},
           {"rebuild_switch_shaders", 2}, {"set_cursor", 3}}) {
    lua_pushinteger(lua, type);
    lua_pushcclosure(lua, guarded<action>, 1);
    lua_setfield(lua, -2, name);
  }
}

}  // namespace vw
