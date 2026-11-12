#include "vw/lua/modules.hpp"
#include "vw/lua_util.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace vw {
namespace {

constexpr lua_Integer default_timeout_ms = 1000;

// Accepts either a raw string payload or a Lua value that is JSON-encoded.
std::string payload_argument(lua_State* lua, int index) {
  if (lua_type(lua, index) == LUA_TSTRING) {
    std::size_t size = 0;
    const char* payload = lua_tolstring(lua, index, &size);
    return {payload, size};
  }
  return to_json(lua, index).dump();
}

int timeout_argument(lua_State* lua, int index) {
  const lua_Integer timeout = luaL_optinteger(lua, index, default_timeout_ms);
  if (timeout <= 0 || timeout > 600000)
    throw std::invalid_argument("socket timeout must be between 1 and 600000 ms");
  return static_cast<int>(timeout);
}

int request_call(lua_State* lua) {
  std::size_t path_size = 0;
  const char* path = luaL_checklstring(lua, 1, &path_size);
  luaL_checkany(lua, 2);
  const std::string result = unix_request({path, path_size}, payload_argument(lua, 2),
                                          timeout_argument(lua, 3));
  lua_pushlstring(lua, result.data(), result.size());
  return 1;
}

int json_call(lua_State* lua) {
  std::size_t path_size = 0;
  const char* path = luaL_checklstring(lua, 1, &path_size);
  luaL_checkany(lua, 2);
  const std::string response = unix_request({path, path_size}, payload_argument(lua, 2),
                                            timeout_argument(lua, 3));
  push_json(lua, nlohmann::json::parse(response));
  return 1;
}

int available_call(lua_State* lua) {
  std::error_code error;
  lua_pushboolean(lua, std::filesystem::is_socket(luaL_checkstring(lua, 1), error));
  return 1;
}

}  // namespace

void install_socket(lua_State* lua) {
  lua_newtable(lua);
  set_function(lua, "available", available_call);
  set_function(lua, "request", guarded<request_call>);
  set_function(lua, "json", guarded<json_call>);
}

}  // namespace vw
