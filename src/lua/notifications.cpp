#include "vw/lua/modules.hpp"
#include "vw/lua_util.hpp"
#include "vw/notifications.hpp"

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

void set_bool(lua_State* lua, const char* field, bool value) {
  lua_pushboolean(lua, value);
  lua_setfield(lua, -2, field);
}

void set_integer(lua_State* lua, const char* field, lua_Integer value) {
  lua_pushinteger(lua, value);
  lua_setfield(lua, -2, field);
}

void push_record(lua_State* lua, const notifications::Notification& record) {
  lua_createtable(lua, 0, 22);
  set_integer(lua, "id", record.id);
  set_string(lua, "app_name", record.app_name);
  set_string(lua, "app_icon", record.app_icon);
  set_string(lua, "summary", record.summary);
  set_string(lua, "body", record.body);
  set_string(lua, "image_path", record.image_path);
  set_string(lua, "icon", record.icon);
  set_string(lua, "image", record.image);
  set_string(lua, "desktop_entry", record.desktop_entry);
  set_string(lua, "category", record.category);
  set_integer(lua, "urgency", record.urgency);
  set_integer(lua, "timestamp", record.timestamp);
  set_integer(lua, "expire_timeout", record.expire_timeout);
  set_integer(lua, "close_reason", record.close_reason);
  set_bool(lua, "resident", record.resident);
  set_bool(lua, "transient", record.transient);
  set_bool(lua, "closed", record.closed);
  set_bool(lua, "popup", record.popup);
  set_bool(lua, "action_icons", record.action_icons);
  set_integer(lua, "value", record.value);
  lua_createtable(lua, static_cast<int>(record.actions.size()), 0);
  lua_Integer index = 1;
  for (const auto& action : record.actions) {
    lua_createtable(lua, 0, 2);
    set_string(lua, "key", action.key);
    set_string(lua, "label", action.label);
    lua_rawseti(lua, -2, index++);
  }
  lua_setfield(lua, -2, "actions");
}

std::uint32_t id_argument(lua_State* lua, int index) {
  const lua_Integer id = luaL_checkinteger(lua, index);
  if (id < 1 || id > std::numeric_limits<std::uint32_t>::max())
    throw std::out_of_range("notification id is out of range");
  return static_cast<std::uint32_t>(id);
}

int start(lua_State* lua) {
  const auto result = notifications::start();
  lua_pushboolean(lua, result.running);
  lua_pushlstring(lua, result.error.data(), result.error.size());
  return 2;
}

// Upvalue 1: 0 open, 1 popups, 2 history.
int records(lua_State* lua) {
  const lua_Integer view = lua_tointeger(lua, lua_upvalueindex(1));
  const auto list = view == 2 ? notifications::history()
                    : view == 1 ? notifications::popups()
                                : notifications::list();
  lua_createtable(lua, static_cast<int>(list.size()), 0);
  lua_Integer index = 1;
  for (const auto& record : list) {
    push_record(lua, record);
    lua_rawseti(lua, -2, index++);
  }
  return 1;
}

int running(lua_State* lua) {
  lua_pushboolean(lua, notifications::running());
  return 1;
}

int dismiss(lua_State* lua) {
  lua_pushboolean(lua, notifications::dismiss(id_argument(lua, 1)));
  return 1;
}

int invoke(lua_State* lua) {
  const std::uint32_t id = id_argument(lua, 1);
  lua_pushboolean(lua, notifications::invoke(id, luaL_checkstring(lua, 2)));
  return 1;
}

int activate(lua_State* lua) {
  lua_pushboolean(lua, notifications::activate(id_argument(lua, 1)));
  return 1;
}

int hide_popup(lua_State* lua) {
  notifications::hide_popup(id_argument(lua, 1));
  return 0;
}

int hide_popups(lua_State*) {
  notifications::hide_popups();
  return 0;
}

int generation(lua_State* lua) {
  lua_pushinteger(lua, static_cast<lua_Integer>(notifications::generation()));
  return 1;
}

int clear(lua_State*) {
  notifications::clear();
  return 0;
}

int dnd(lua_State* lua) {
  lua_pushboolean(lua, notifications::do_not_disturb());
  return 1;
}

int set_dnd(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TBOOLEAN);
  notifications::set_do_not_disturb(lua_toboolean(lua, 1) != 0);
  return 0;
}

}  // namespace

void install_notifications(lua_State* lua) {
  lua_newtable(lua);
  for (const auto& [name, function] : {
      std::pair<const char*, lua_CFunction>{"start", guarded<start>}, {"running", guarded<running>},
      {"dismiss", guarded<dismiss>}, {"invoke", guarded<invoke>}, {"clear", guarded<clear>},
      {"dnd", guarded<dnd>}, {"set_dnd", guarded<set_dnd>}, {"activate", guarded<activate>},
      {"hide_popup", guarded<hide_popup>}, {"hide_popups", guarded<hide_popups>},
      {"generation", guarded<generation>}}) {
    set_function(lua, name, function);
  }
  for (const auto& [view, name] : {std::pair{0, "list"}, {1, "popups"}, {2, "history"}}) {
    lua_pushinteger(lua, view);
    lua_pushcclosure(lua, guarded<records>, 1);
    lua_setfield(lua, -2, name);
  }
}

}  // namespace vw
