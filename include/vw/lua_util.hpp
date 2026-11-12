#pragma once

#include "vw/ipc.hpp"

#include <lua.hpp>
#include <nlohmann/json.hpp>

#include <exception>
#include <string>
#include <string_view>

namespace vw {

// Lua is built as C, so lua_error unwinds with longjmp. A C++ exception must
// never cross a Lua frame, and lua_error must not be raised from inside a
// catch handler. Every binding that can throw is registered through guarded<>,
// which converts the exception into a Lua error after the handler has exited.
template <lua_CFunction Function>
int guarded(lua_State* lua) {
  try {
    return Function(lua);
  } catch (const std::exception& error) {
    lua_pushstring(lua, error.what());
  } catch (...) {
    lua_pushliteral(lua, "unknown C++ exception");
  }
  return lua_error(lua);
}

inline void set_function(lua_State* lua, const char* name, lua_CFunction function) {
  lua_pushcfunction(lua, function);
  lua_setfield(lua, -2, name);
}

// JSON null maps to nil. Empty Lua tables encode as objects; tables whose keys
// are exactly 1..n encode as arrays.
void push_json(lua_State* lua, const nlohmann::json& value);
nlohmann::json to_json(lua_State* lua, int index);

// Pushes the vw.json module table.
void install_json(lua_State* lua);

}  // namespace vw
