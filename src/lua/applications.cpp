#include "vw/applications.hpp"
#include "vw/lua/modules.hpp"
#include "vw/lua_util.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace vw {
namespace {

void set_string(lua_State* lua, const char* key, const std::string& value) {
  lua_pushlstring(lua, value.data(), value.size());
  lua_setfield(lua, -2, key);
}

void push_strings(lua_State* lua, const std::vector<std::string>& values) {
  lua_createtable(lua, static_cast<int>(values.size()), 0);
  lua_Integer index = 1;
  for (const std::string& value : values) {
    lua_pushlstring(lua, value.data(), value.size());
    lua_rawseti(lua, -2, index++);
  }
}

void push_application(lua_State* lua, const applications::Application& app) {
  lua_createtable(lua, 0, 16);
  set_string(lua, "id", app.id);
  set_string(lua, "name", app.name);
  set_string(lua, "display_name", app.display_name);
  set_string(lua, "generic_name", app.generic_name);
  set_string(lua, "description", app.description);
  set_string(lua, "executable", app.executable);
  set_string(lua, "commandline", app.commandline);
  set_string(lua, "filename", app.filename);
  set_string(lua, "startup_wm_class", app.startup_wm_class);
  set_string(lua, "icon", app.icon);
  set_string(lua, "icon_path", app.icon_path);
  if (!app.icon_path.empty()) {
    lua_createtable(lua, 0, 3);
    set_string(lua, "path", app.icon_path);
    lua_pushinteger(lua, 48);
    lua_setfield(lua, -2, "width");
    lua_pushinteger(lua, 48);
    lua_setfield(lua, -2, "height");
    lua_setfield(lua, -2, "icon_image");
  }
  lua_pushboolean(lua, app.terminal);
  lua_setfield(lua, -2, "terminal");
  push_strings(lua, app.categories);
  lua_setfield(lua, -2, "categories");
  push_strings(lua, app.keywords);
  lua_setfield(lua, -2, "keywords");
}

int list(lua_State* lua) {
  const char* query = lua_isnoneornil(lua, 1) ? "" : luaL_checkstring(lua, 1);
  const lua_Integer limit = luaL_optinteger(lua, 2, 0);
  if (limit < 0) throw std::invalid_argument("application result limit cannot be negative");
  const auto matches = applications::search(query, static_cast<std::size_t>(limit));
  lua_createtable(lua, static_cast<int>(matches.size()), 0);
  lua_Integer index = 1;
  for (const auto& app : matches) {
    push_application(lua, app);
    lua_rawseti(lua, -2, index++);
  }
  return 1;
}

int launch(lua_State* lua) {
  applications::launch(luaL_checkstring(lua, 1));
  lua_pushboolean(lua, 1);
  return 1;
}

}  // namespace

void install_applications(lua_State* lua) {
  lua_newtable(lua);
  set_function(lua, "list", guarded<list>);
  set_function(lua, "search", guarded<list>);
  set_function(lua, "launch", guarded<launch>);
}

}  // namespace vw
