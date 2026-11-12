#include "vw/animation.hpp"
#include "vw/lua/modules.hpp"
#include "vw/lua_util.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace vw {
namespace {

double number_option(lua_State* lua, const char* key, double fallback) {
  lua_getfield(lua, 3, key);
  const double value = lua_isnil(lua, -1) ? fallback : luaL_checknumber(lua, -1);
  lua_pop(lua, 1);
  return value;
}

// vw.animation.value(id, target, {duration, initial, easing, bezier, mass,
// stiffness, damping, min, max})
int value(lua_State* lua) {
  const std::string id(luaL_checkstring(lua, 1));
  const double target = luaL_checknumber(lua, 2);
  AnimationOptions options;
  if (!lua_isnoneornil(lua, 3)) {
    luaL_checktype(lua, 3, LUA_TTABLE);
    options.duration = number_option(lua, "duration", options.duration);
    lua_getfield(lua, 3, "initial");
    if (!lua_isnil(lua, -1)) options.initial = luaL_checknumber(lua, -1);
    lua_pop(lua, 1);
    lua_getfield(lua, 3, "easing");
    const std::string mode = lua_isnil(lua, -1) ? "out_cubic" : luaL_checkstring(lua, -1);
    lua_pop(lua, 1);
    const auto easing = parse_easing(mode);
    if (!easing) throw std::invalid_argument("unknown animation easing: " + mode);
    options.easing = *easing;
    lua_getfield(lua, 3, "bezier");
    if (!lua_isnil(lua, -1)) {
      luaL_checktype(lua, -1, LUA_TTABLE);
      for (int index = 0; index < 4; ++index) {
        lua_geti(lua, -1, index + 1);
        options.bezier[static_cast<std::size_t>(index)] = luaL_checknumber(lua, -1);
        lua_pop(lua, 1);
      }
      options.easing = Easing::bezier;
    }
    lua_pop(lua, 1);
    options.mass = number_option(lua, "mass", options.mass);
    options.stiffness = number_option(lua, "stiffness", options.stiffness);
    options.damping = number_option(lua, "damping", options.damping);
    options.min = number_option(lua, "min", options.min);
    options.max = number_option(lua, "max", options.max);
  }
  lua_pushnumber(lua, animate(id, target, options));
  return 1;
}

int request_frame(lua_State*) {
  request_animation_continuation();
  return 0;
}

}  // namespace

void install_animation(lua_State* lua) {
  lua_newtable(lua);
  set_function(lua, "value", guarded<value>);
  set_function(lua, "request_frame", request_frame);
}

}  // namespace vw
