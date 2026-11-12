#include "vw/lua/modules.hpp"
#include "vw/lua_util.hpp"
#include "vw/pipewire.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace vw {
namespace {

void set_string(lua_State* lua, const char* field, const std::string& value) {
  lua_pushlstring(lua, value.data(), value.size());
  lua_setfield(lua, -2, field);
}

void push_node(lua_State* lua, const pipewire::Node& node) {
  lua_createtable(lua, 0, 13);
  lua_pushinteger(lua, node.id);
  lua_setfield(lua, -2, "id");
  set_string(lua, "name", node.name);
  set_string(lua, "description", node.description);
  set_string(lua, "media_class", node.media_class);
  set_string(lua, "app_name", node.app_name);
  set_string(lua, "state", node.state);
  lua_pushnumber(lua, node.volume());
  lua_setfield(lua, -2, "volume");
  lua_pushnumber(lua, node.linear_volume());
  lua_setfield(lua, -2, "linear_volume");
  lua_pushboolean(lua, node.muted);
  lua_setfield(lua, -2, "muted");
  lua_pushboolean(lua, node.volume_known);
  lua_setfield(lua, -2, "volume_known");
  lua_pushboolean(lua, node.can_control);
  lua_setfield(lua, -2, "can_control");
  lua_pushinteger(lua, static_cast<lua_Integer>(node.channel_volumes.size()));
  lua_setfield(lua, -2, "channels");
  lua_createtable(lua, static_cast<int>(node.channel_volumes.size()), 0);
  for (std::size_t index = 0; index < node.channel_volumes.size(); ++index) {
    lua_pushnumber(lua, node.channel_volumes[index]);
    lua_rawseti(lua, -2, static_cast<lua_Integer>(index + 1));
  }
  lua_setfield(lua, -2, "channel_volumes");
}

int available(lua_State* lua) {
  lua_pushboolean(lua, pipewire::available());
  return 1;
}

int nodes(lua_State* lua) {
  const auto filter = static_cast<pipewire::Filter>(lua_tointeger(lua, lua_upvalueindex(1)));
  const auto list = pipewire::nodes(filter);
  lua_createtable(lua, static_cast<int>(list.size()), 0);
  lua_Integer index = 1;
  for (const auto& node : list) {
    push_node(lua, node);
    lua_rawseti(lua, -2, index++);
  }
  return 1;
}

int default_node(lua_State* lua) {
  const auto node = lua_toboolean(lua, lua_upvalueindex(1)) ? pipewire::default_source()
                                                            : pipewire::default_sink();
  if (node) push_node(lua, *node);
  else lua_pushnil(lua);
  return 1;
}

int control(lua_State* lua) {
  const lua_Integer id = luaL_checkinteger(lua, 1);
  if (id < 0 || id > std::numeric_limits<std::uint32_t>::max())
    throw std::out_of_range("PipeWire node id is out of range");
  const auto node = static_cast<std::uint32_t>(id);
  pipewire::Result result;
  switch (lua_tointeger(lua, lua_upvalueindex(1))) {
    case 0: result = pipewire::set_volume(node, luaL_checknumber(lua, 2)); break;
    case 1:
      luaL_checktype(lua, 2, LUA_TBOOLEAN);
      result = pipewire::set_mute(node, lua_toboolean(lua, 2) != 0);
      break;
    case 2: result = pipewire::toggle_mute(node); break;
    case 3: result = pipewire::set_default_sink(node); break;
    default: result = pipewire::set_default_source(node); break;
  }
  lua_pushboolean(lua, result.ok);
  lua_pushlstring(lua, result.error.data(), result.error.size());
  return 2;
}

int error(lua_State* lua) {
  const auto message = pipewire::error();
  lua_pushlstring(lua, message.data(), message.size());
  return 1;
}

}  // namespace

void install_pipewire(lua_State* lua) {
  lua_newtable(lua);
  set_function(lua, "available", guarded<available>);
  set_function(lua, "error", guarded<error>);
  for (const auto& [name, kind] : {std::pair{"nodes", 0}, {"sinks", 1}, {"sources", 2}, {"streams", 3}}) {
    lua_pushinteger(lua, kind);
    lua_pushcclosure(lua, guarded<nodes>, 1);
    lua_setfield(lua, -2, name);
  }
  for (const bool input : {false, true}) {
    lua_pushboolean(lua, input);
    lua_pushcclosure(lua, guarded<default_node>, 1);
    lua_setfield(lua, -2, input ? "default_source" : "default_sink");
  }
  for (const auto& [name, action] : {std::pair{"set_volume", 0}, {"set_mute", 1}, {"toggle_mute", 2},
                                    {"set_default_sink", 3}, {"set_default_source", 4}}) {
    lua_pushinteger(lua, action);
    lua_pushcclosure(lua, guarded<control>, 1);
    lua_setfield(lua, -2, name);
  }
}

}  // namespace vw
