#include "vw/lua_util.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace vw {
namespace {

constexpr int max_json_depth = 128;

void push_json_value(lua_State* lua, const nlohmann::json& value, int depth) {
  if (depth > max_json_depth || !lua_checkstack(lua, 3))
    throw std::runtime_error("JSON value is nested too deeply");
  using Type = nlohmann::json::value_t;
  switch (value.type()) {
    case Type::null:
    case Type::discarded:
      lua_pushnil(lua);
      return;
    case Type::boolean:
      lua_pushboolean(lua, value.get<bool>());
      return;
    case Type::number_integer:
      lua_pushinteger(lua, value.get<lua_Integer>());
      return;
    case Type::number_unsigned:
      lua_pushinteger(lua, static_cast<lua_Integer>(value.get<std::uint64_t>()));
      return;
    case Type::number_float:
      lua_pushnumber(lua, value.get<lua_Number>());
      return;
    case Type::string: {
      const auto& text = value.get_ref<const std::string&>();
      lua_pushlstring(lua, text.data(), text.size());
      return;
    }
    case Type::array: {
      lua_createtable(lua, static_cast<int>(value.size()), 0);
      lua_Integer index = 1;
      for (const auto& item : value) {
        push_json_value(lua, item, depth + 1);
        lua_seti(lua, -2, index++);
      }
      return;
    }
    case Type::object: {
      lua_createtable(lua, 0, static_cast<int>(value.size()));
      for (auto item = value.begin(); item != value.end(); ++item) {
        lua_pushlstring(lua, item.key().data(), item.key().size());
        push_json_value(lua, item.value(), depth + 1);
        lua_rawset(lua, -3);
      }
      return;
    }
    case Type::binary:
      throw std::runtime_error("binary JSON values are not supported");
  }
}

nlohmann::json to_json_value(lua_State* lua, int index, int depth) {
  if (depth > max_json_depth)
    throw std::runtime_error("cannot encode a cyclic or deeply nested table");
  index = lua_absindex(lua, index);
  switch (lua_type(lua, index)) {
    case LUA_TNONE:
    case LUA_TNIL:
      return nullptr;
    case LUA_TBOOLEAN:
      return lua_toboolean(lua, index) != 0;
    case LUA_TNUMBER: {
      if (lua_isinteger(lua, index)) return lua_tointeger(lua, index);
      const lua_Number number = lua_tonumber(lua, index);
      if (!std::isfinite(number)) throw std::runtime_error("cannot encode a non-finite number");
      return number;
    }
    case LUA_TSTRING: {
      std::size_t size = 0;
      const char* text = lua_tolstring(lua, index, &size);
      return std::string(text, size);
    }
    case LUA_TTABLE: {
      if (!lua_checkstack(lua, 4)) throw std::runtime_error("Lua stack overflow while encoding");
      const lua_Unsigned length = lua_rawlen(lua, index);
      lua_Unsigned count = 0;
      bool sequence = true;
      lua_pushnil(lua);
      while (lua_next(lua, index) != 0) {
        ++count;
        if (!lua_isinteger(lua, -2)) sequence = false;
        else {
          const lua_Integer key = lua_tointeger(lua, -2);
          if (key < 1 || static_cast<lua_Unsigned>(key) > length) sequence = false;
        }
        lua_pop(lua, 1);
      }
      if (sequence && count > 0 && count == length) {
        auto array = nlohmann::json::array();
        for (lua_Unsigned item = 1; item <= length; ++item) {
          lua_rawgeti(lua, index, static_cast<lua_Integer>(item));
          array.push_back(to_json_value(lua, -1, depth + 1));
          lua_pop(lua, 1);
        }
        return array;
      }
      auto object = nlohmann::json::object();
      lua_pushnil(lua);
      while (lua_next(lua, index) != 0) {
        std::string key;
        if (lua_type(lua, -2) == LUA_TSTRING) {
          std::size_t size = 0;
          const char* text = lua_tolstring(lua, -2, &size);
          key.assign(text, size);
        } else if (lua_isinteger(lua, -2)) {
          key = std::to_string(lua_tointeger(lua, -2));
        } else {
          throw std::runtime_error("JSON object keys must be strings or integers");
        }
        object[std::move(key)] = to_json_value(lua, -1, depth + 1);
        lua_pop(lua, 1);
      }
      return object;
    }
    default:
      throw std::runtime_error(std::string("cannot encode a Lua ") +
                               luaL_typename(lua, index) + " as JSON");
  }
}

int json_encode(lua_State* lua) {
  luaL_checkany(lua, 1);
  bool pretty = false;
  if (!lua_isnoneornil(lua, 2)) {
    luaL_checktype(lua, 2, LUA_TTABLE);
    lua_getfield(lua, 2, "pretty");
    pretty = lua_toboolean(lua, -1) != 0;
    lua_pop(lua, 1);
  }
  const std::string encoded = to_json(lua, 1).dump(
      pretty ? 2 : -1, ' ', false, nlohmann::json::error_handler_t::replace);
  lua_pushlstring(lua, encoded.data(), encoded.size());
  return 1;
}

int json_decode(lua_State* lua) {
  std::size_t size = 0;
  const char* text = luaL_checklstring(lua, 1, &size);
  push_json(lua, nlohmann::json::parse(text, text + size));
  return 1;
}

}  // namespace

void push_json(lua_State* lua, const nlohmann::json& value) {
  push_json_value(lua, value, 0);
}

nlohmann::json to_json(lua_State* lua, int index) {
  return to_json_value(lua, index, 0);
}

void install_json(lua_State* lua) {
  lua_newtable(lua);
  set_function(lua, "encode", guarded<json_encode>);
  set_function(lua, "decode", guarded<json_decode>);
}

}  // namespace vw
