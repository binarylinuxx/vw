// Deprecated Lua frontend. Every vw.base.* function parses its table into the
// matching vw::Ui props struct; all scene logic lives in src/ui.cpp.
#include "vw/lua_config.hpp"
#include "vw/animation.hpp"
#include "vw/host.hpp"
#include "vw/hyprland.hpp"
#include "vw/lua/modules.hpp"
#include "vw/lua_util.hpp"
#include "vw/notifications.hpp"
#include "vw/pipewire.hpp"
#include "vw/process.hpp"
#include "vw/ui.hpp"
#include "vw/zwwm.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef VW_VERSION
#define VW_VERSION "unknown"
#endif

namespace vw {
namespace {

constexpr const char* widget_type = "vw.widget";
constexpr const char* context_type = "vw.context";
constexpr const char* context_key = "vw.context.instance";

// One per Lua state. ui is only set while the configuration runs, so a
// callback that tries to build widgets later gets an error instead of a
// dangling scene.
struct Context {
  Ui* ui = nullptr;
  std::weak_ptr<lua_State> state;
};

Context& context(lua_State* lua) {
  return *static_cast<Context*>(lua_touserdata(lua, lua_upvalueindex(1)));
}

Ui& current_ui(lua_State* lua) {
  Ui* ui = context(lua).ui;
  if (ui == nullptr)
    throw std::logic_error("vw.base functions can only be called while the configuration runs");
  return *ui;
}

int destroy_context(lua_State* lua) {
  static_cast<Context*>(lua_touserdata(lua, 1))->~Context();
  return 0;
}

// --- table fields ----------------------------------------------------------

bool has_field(lua_State* lua, int table, const char* name) {
  lua_getfield(lua, table, name);
  const bool present = !lua_isnil(lua, -1);
  lua_pop(lua, 1);
  return present;
}

float number_field(lua_State* lua, int table, const char* name, float fallback) {
  lua_getfield(lua, table, name);
  const float value = lua_isnil(lua, -1) ? fallback : static_cast<float>(luaL_checknumber(lua, -1));
  lua_pop(lua, 1);
  return value;
}

std::optional<float> optional_number(lua_State* lua, int table, const char* name) {
  if (!has_field(lua, table, name)) return std::nullopt;
  return number_field(lua, table, name, 0.0F);
}

bool boolean_field(lua_State* lua, int table, const char* name, bool fallback) {
  lua_getfield(lua, table, name);
  const bool value = lua_isnil(lua, -1) ? fallback : lua_toboolean(lua, -1) != 0;
  lua_pop(lua, 1);
  return value;
}

std::string string_field(lua_State* lua, int table, const char* name, std::string fallback = {}) {
  lua_getfield(lua, table, name);
  if (!lua_isnil(lua, -1)) {
    std::size_t size = 0;
    const char* text = luaL_checklstring(lua, -1, &size);
    fallback.assign(text, size);
  }
  lua_pop(lua, 1);
  return fallback;
}

lua_Integer integer_field(lua_State* lua, int table, const char* name, lua_Integer fallback) {
  lua_getfield(lua, table, name);
  const lua_Integer value = lua_isnil(lua, -1) ? fallback : luaL_checkinteger(lua, -1);
  lua_pop(lua, 1);
  return value;
}

// Components that are present override the matching channels of value.
void color_field(lua_State* lua, int table, const char* name, Color& value) {
  lua_getfield(lua, table, name);
  if (!lua_isnil(lua, -1)) {
    luaL_checktype(lua, -1, LUA_TTABLE);
    const int color = lua_gettop(lua);
    for (int index = 1; index <= 4; ++index) {
      lua_geti(lua, color, index);
      if (!lua_isnil(lua, -1))
        value[static_cast<std::size_t>(index - 1)] = static_cast<float>(luaL_checknumber(lua, -1));
      lua_pop(lua, 1);
    }
  }
  lua_pop(lua, 1);
}

std::optional<Color> optional_color(lua_State* lua, int table, const char* name, Color fallback) {
  if (!has_field(lua, table, name)) return std::nullopt;
  color_field(lua, table, name, fallback);
  return fallback;
}

std::optional<std::uint32_t> anchor_field(lua_State* lua, int table) {
  if (!has_field(lua, table, "anchor")) return std::nullopt;
  const std::string value = string_field(lua, table, "anchor");
  if (value == "center") return anchor::center;
  if (value == "top") return anchor::top;
  if (value == "bottom") return anchor::bottom;
  if (value == "left") return anchor::left;
  if (value == "right") return anchor::right;
  if (value == "top_left") return anchor::top_left;
  if (value == "top_right") return anchor::top_right;
  if (value == "bottom_left") return anchor::bottom_left;
  if (value == "bottom_right") return anchor::bottom_right;
  if (value == "fill") return anchor::fill;
  throw std::invalid_argument("invalid anchor '" + value + "'");
}

Layer layer_field(lua_State* lua, int table) {
  const std::string value = string_field(lua, table, "layer", "top");
  if (value == "background") return Layer::background;
  if (value == "bottom") return Layer::bottom;
  if (value == "top") return Layer::top;
  if (value == "overlay") return Layer::overlay;
  throw std::invalid_argument("invalid layer '" + value + "'");
}

Margin margin_field(lua_State* lua, int table) {
  Margin margin{};
  lua_getfield(lua, table, "margin");
  if (lua_isnumber(lua, -1)) {
    margin.fill(static_cast<std::int32_t>(lua_tointeger(lua, -1)));
  } else if (!lua_isnil(lua, -1)) {
    luaL_checktype(lua, -1, LUA_TTABLE);
    for (int index = 1; index <= 4; ++index) {
      lua_geti(lua, -1, index);
      if (!lua_isnil(lua, -1))
        margin[static_cast<std::size_t>(index - 1)] =
            static_cast<std::int32_t>(luaL_checkinteger(lua, -1));
      lua_pop(lua, 1);
    }
  }
  lua_pop(lua, 1);
  return margin;
}

int weight_field(lua_State* lua, int table) {
  lua_getfield(lua, table, "weight");
  if (lua_isinteger(lua, -1)) {
    const int value = static_cast<int>(lua_tointeger(lua, -1));
    lua_pop(lua, 1);
    return value;
  }
  lua_pop(lua, 1);
  const std::string value = string_field(lua, table, "weight", "regular");
  if (value == "thin") return weight::thin;
  if (value == "light") return weight::light;
  if (value == "regular" || value == "normal") return weight::regular;
  if (value == "medium") return weight::medium;
  if (value == "semibold") return weight::semibold;
  if (value == "bold") return weight::bold;
  if (value == "black") return weight::black;
  throw std::invalid_argument("invalid text weight '" + value + "'");
}

Placement placement(lua_State* lua, int table) {
  Placement place;
  place.x = optional_number(lua, table, "x");
  place.y = optional_number(lua, table, "y");
  place.anchor = anchor_field(lua, table);
  place.layer = layer_field(lua, table);
  place.margin = margin_field(lua, table);
  lua_getfield(lua, table, "exclusive");
  if (lua_isinteger(lua, -1)) place.exclusive_zone = static_cast<std::int32_t>(lua_tointeger(lua, -1));
  else place.exclusive = lua_toboolean(lua, -1) != 0;
  lua_pop(lua, 1);
  place.visible = boolean_field(lua, table, "visible", true);
  lua_getfield(lua, table, "clip");
  if (!lua_isnil(lua, -1)) {
    luaL_checktype(lua, -1, LUA_TTABLE);
    std::array<float, 4> values{};
    for (int index = 0; index < 4; ++index) {
      lua_geti(lua, -1, index + 1);
      values[static_cast<std::size_t>(index)] = static_cast<float>(luaL_checknumber(lua, -1));
      lua_pop(lua, 1);
    }
    place.clip = Clip{values[0], values[1], values[2], values[3],
                      number_field(lua, table, "clip_radius", 0.0F)};
  }
  lua_pop(lua, 1);
  return place;
}

// --- callbacks -------------------------------------------------------------

// A Lua function that keeps its state alive for as long as a scene holds it.
class LuaFunction {
 public:
  LuaFunction(std::shared_ptr<lua_State> state, int ref) : state_(std::move(state)), ref_(ref) {}
  LuaFunction(const LuaFunction&) = delete;
  LuaFunction& operator=(const LuaFunction&) = delete;

  // push() pushes the arguments and returns how many; nresults stay on the
  // stack for the caller to pop.
  void call(const std::function<int(lua_State*)>& push, int nresults = 0) const {
    lua_State* lua = state_.get();
    lua_rawgeti(lua, LUA_REGISTRYINDEX, ref_);
    const int arguments = push(lua);
    if (lua_pcall(lua, arguments, nresults, 0) != LUA_OK) {
      const char* message = lua_tostring(lua, -1);
      std::string error = message == nullptr ? "Lua callback failed" : message;
      lua_pop(lua, 1);
      throw std::runtime_error(error);
    }
  }

 private:
  std::shared_ptr<lua_State> state_;
  int ref_;
};

using FunctionRef = std::shared_ptr<const LuaFunction>;

FunctionRef function_field(lua_State* lua, int table, const char* name) {
  lua_getfield(lua, table, name);
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return nullptr;
  }
  luaL_checktype(lua, -1, LUA_TFUNCTION);
  auto state = context(lua).state.lock();
  if (!state) throw std::logic_error("Lua state is closing");
  return std::make_shared<const LuaFunction>(std::move(state), luaL_ref(lua, LUA_REGISTRYINDEX));
}

void set_number(lua_State* lua, const char* name, double value) {
  lua_pushnumber(lua, value);
  lua_setfield(lua, -2, name);
}

ClickHandler click_handler(FunctionRef function) {
  if (!function) return {};
  return [function](const ClickEvent& event) {
    function->call([&](lua_State* lua) {
      lua_createtable(lua, 0, 3);
      set_number(lua, "x", event.x);
      set_number(lua, "y", event.y);
      lua_pushinteger(lua, event.button);
      lua_setfield(lua, -2, "button");
      return 1;
    });
  };
}

DragHandler drag_handler(FunctionRef function) {
  if (!function) return {};
  return [function](const DragEvent& event) {
    function->call([&](lua_State* lua) {
      lua_createtable(lua, 0, 10);
      set_number(lua, "x", event.x);
      set_number(lua, "y", event.y);
      set_number(lua, "start_x", event.start_x);
      set_number(lua, "start_y", event.start_y);
      set_number(lua, "dx", event.dx);
      set_number(lua, "dy", event.dy);
      set_number(lua, "delta_x", event.delta_x);
      set_number(lua, "delta_y", event.delta_y);
      lua_pushinteger(lua, event.button);
      lua_setfield(lua, -2, "button");
      lua_pushboolean(lua, event.cancelled);
      lua_setfield(lua, -2, "cancelled");
      return 1;
    });
  };
}

TextHandler text_handler(FunctionRef function) {
  if (!function) return {};
  return [function](const std::string& value) {
    function->call([&](lua_State* lua) {
      lua_pushlstring(lua, value.data(), value.size());
      return 1;
    });
  };
}

AuthenticationHandler authentication_handler(FunctionRef function) {
  if (!function) return {};
  return [function](bool authenticated) {
    function->call([&](lua_State* lua) {
      lua_pushboolean(lua, authenticated);
      return 1;
    });
  };
}

FocusHandler focus_handler(FunctionRef function) {
  if (!function) return {};
  return [function](const FocusEvent& event) {
    function->call([&](lua_State* lua) {
      lua_createtable(lua, 0, 4);
      lua_pushboolean(lua, event.active);
      lua_setfield(lua, -2, "active");
      lua_pushstring(lua, event.active ? "activated" : "dismissed");
      lua_setfield(lua, -2, "type");
      if (!event.active) {
        const std::string_view reason = event.reason();
        lua_pushlstring(lua, reason.data(), reason.size());
        lua_setfield(lua, -2, "reason");
        lua_pushinteger(lua, event.code);
        lua_setfield(lua, -2, "code");
      }
      return 1;
    });
  };
}

Input input(lua_State* lua, int table) {
  Input result;
  result.on_click = click_handler(function_field(lua, table, "on_click"));
  lua_getfield(lua, table, "hover");
  if (!lua_isnil(lua, -1)) {
    luaL_checktype(lua, -1, LUA_TTABLE);
    const int hover = lua_gettop(lua);
    Hover value;
    value.scale = number_field(lua, hover, "scale", 1.0F);
    value.duration = number_field(lua, hover, "duration", 0.16F);
    value.color = optional_color(lua, hover, "color", Color{0, 0, 0, 0});
    result.hover = value;
  }
  lua_pop(lua, 1);
  result.on_drag_start = drag_handler(function_field(lua, table, "on_drag_start"));
  result.on_drag = drag_handler(function_field(lua, table, "on_drag"));
  result.on_drag_end = drag_handler(function_field(lua, table, "on_drag_end"));
  const lua_Integer button = integer_field(lua, table, "drag_button", 272);
  if (button < 0 || button > std::numeric_limits<std::uint32_t>::max())
    throw std::invalid_argument("drag_button is out of range");
  result.drag_button = static_cast<std::uint32_t>(button);
  return result;
}

// --- widgets ---------------------------------------------------------------

int push_widget(lua_State* lua, const Widget& widget) {
  new (lua_newuserdatauv(lua, sizeof(Widget), 0)) Widget(widget);
  luaL_setmetatable(lua, widget_type);
  return 1;
}

const Widget* test_widget(lua_State* lua, int index) {
  return static_cast<const Widget*>(luaL_testudata(lua, index, widget_type));
}

// Widgets and (nested) arrays of widgets, as produced by repeaters.
void collect_children(lua_State* lua, int index, std::vector<Widget>& children) {
  index = lua_absindex(lua, index);
  if (const Widget* widget = test_widget(lua, index)) {
    children.push_back(*widget);
    return;
  }
  if (!lua_istable(lua, index)) throw std::invalid_argument("layout child must be a widget or repeater");
  const lua_Unsigned count = lua_rawlen(lua, index);
  for (lua_Unsigned child = 1; child <= count; ++child) {
    lua_rawgeti(lua, index, static_cast<lua_Integer>(child));
    collect_children(lua, -1, children);
    lua_pop(lua, 1);
  }
}

std::vector<Widget> children_field(lua_State* lua, int table, const char* name) {
  std::vector<Widget> children;
  lua_getfield(lua, table, name);
  if (!lua_isnil(lua, -1)) collect_children(lua, -1, children);
  lua_pop(lua, 1);
  return children;
}

// Calls table[name]() when it is a function and leaves the result on the stack.
void resolve_field(lua_State* lua, int table, const char* name) {
  lua_getfield(lua, table, name);
  if (lua_isfunction(lua, -1) && lua_pcall(lua, 0, 1, 0) != LUA_OK) {
    std::string error = lua_tostring(lua, -1) == nullptr ? "Lua error" : lua_tostring(lua, -1);
    lua_pop(lua, 1);
    throw std::runtime_error(error);
  }
}

int add_rectangle(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  RectangleProps props;
  props.width = number_field(lua, 1, "width", props.width);
  props.height = number_field(lua, 1, "height", props.height);
  props.radius = number_field(lua, 1, "radius", props.radius);
  constexpr std::array corners{"top_left_radius", "top_right_radius", "bottom_right_radius",
                               "bottom_left_radius"};
  for (std::size_t index = 0; index < corners.size(); ++index)
    props.corner_radii[index] = optional_number(lua, 1, corners[index]);
  props.border_width = number_field(lua, 1, "border_width", props.border_width);
  color_field(lua, 1, "color", props.color);
  props.color2 = optional_color(lua, 1, "color2", props.color);
  color_field(lua, 1, "border_color", props.border_color);
  const std::string gradient = string_field(lua, 1, "gradient", "none");
  if (gradient == "horizontal") props.gradient = Gradient::horizontal;
  else if (gradient == "vertical") props.gradient = Gradient::vertical;
  else if (gradient != "none") throw std::invalid_argument("invalid gradient '" + gradient + "'");
  props.shader = string_field(lua, 1, "shader");
  lua_getfield(lua, 1, "shader_params");
  if (!lua_isnil(lua, -1)) {
    luaL_checktype(lua, -1, LUA_TTABLE);
    for (int index = 0; index < 4; ++index) {
      lua_geti(lua, -1, index + 1);
      if (!lua_isnil(lua, -1))
        props.shader_params[static_cast<std::size_t>(index)] =
            static_cast<float>(luaL_checknumber(lua, -1));
      lua_pop(lua, 1);
    }
  }
  lua_pop(lua, 1);
  props.place = placement(lua, 1);
  props.input = input(lua, 1);
  return push_widget(lua, current_ui(lua).rectangle(props));
}

int add_hit_area(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  HitAreaProps props;
  props.width = number_field(lua, 1, "width", 0.0F);
  props.height = number_field(lua, 1, "height", 0.0F);
  props.place = placement(lua, 1);
  props.input = input(lua, 1);
  return push_widget(lua, current_ui(lua).hit_area(props));
}

int add_text(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  TextProps props;
  lua_getfield(lua, 1, "text");
  props.text = luaL_checkstring(lua, -1);
  lua_pop(lua, 1);
  props.font = string_field(lua, 1, "font", props.font);
  props.size = number_field(lua, 1, "size", props.size);
  props.weight = weight_field(lua, 1);
  color_field(lua, 1, "color", props.color);
  props.max_width = number_field(lua, 1, "max_width", 0.0F);
  props.max_lines = static_cast<int>(integer_field(lua, 1, "max_lines", 0));
  props.align = string_field(lua, 1, "align", props.align);
  props.ellipsize = string_field(lua, 1, "ellipsize", props.ellipsize);
  props.radius = number_field(lua, 1, "radius", 0.0F);
  props.place = placement(lua, 1);
  props.input = input(lua, 1);
  return push_widget(lua, current_ui(lua).text(props));
}

ImageProps image_props(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  ImageProps props;
  props.path = string_field(lua, 1, "path");
  props.icon = string_field(lua, 1, "icon");
  props.width = number_field(lua, 1, "width", 0.0F);
  props.height = number_field(lua, 1, "height", 0.0F);
  const std::string fit = string_field(lua, 1, "fit", "stretch");
  if (fit == "contain") props.fit = ImageFit::contain;
  else if (fit == "cover") props.fit = ImageFit::cover;
  else if (fit != "stretch") throw std::invalid_argument("image fit must be stretch, contain or cover");
  props.radius = number_field(lua, 1, "radius", 0.0F);
  color_field(lua, 1, "tint", props.tint);
  props.effect.saturation = number_field(lua, 1, "saturation", 1.0F);
  props.effect.hue_rotate = number_field(lua, 1, "hue_rotate", 0.0F);
  color_field(lua, 1, "colorize", props.effect.colorize);
  props.effect.colorize_amount = number_field(lua, 1, "colorize_amount", 0.0F);
  lua_getfield(lua, 1, "mask");
  if (lua_istable(lua, -1)) {
    const lua_Unsigned count = lua_rawlen(lua, -1);
    props.mask.reserve(static_cast<std::size_t>(count));
    for (lua_Unsigned index = 1; index <= count; ++index) {
      lua_rawgeti(lua, -1, static_cast<lua_Integer>(index));
      props.mask.push_back(static_cast<float>(luaL_checknumber(lua, -1)));
      lua_pop(lua, 1);
    }
  }
  lua_pop(lua, 1);
  props.place = placement(lua, 1);
  props.input = input(lua, 1);
  return props;
}

int add_image(lua_State* lua) {
  return push_widget(lua, current_ui(lua).image(image_props(lua)));
}

int add_canvas(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  CanvasProps props;
  lua_getfield(lua, 1, "id");
  props.id = luaL_checkstring(lua, -1);
  lua_pop(lua, 1);
  props.code = string_field(lua, 1, "code");
  props.script = string_field(lua, 1, "script");
  props.width = number_field(lua, 1, "width", props.width);
  props.height = number_field(lua, 1, "height", props.height);
  props.radius = number_field(lua, 1, "radius", 0.0F);
  props.opacity = number_field(lua, 1, "opacity", 1.0F);
  props.animate = boolean_field(lua, 1, "animate", false);
  props.place = placement(lua, 1);
  props.input = input(lua, 1);
  return push_widget(lua, current_ui(lua).canvas(props));
}

int add_text_field(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  TextFieldProps props;
  lua_getfield(lua, 1, "id");
  props.id = luaL_checkstring(lua, -1);
  lua_pop(lua, 1);
  props.value = string_field(lua, 1, "value");
  props.placeholder = string_field(lua, 1, "placeholder");
  props.width = number_field(lua, 1, "width", props.width);
  props.height = number_field(lua, 1, "height", props.height);
  props.radius = number_field(lua, 1, "radius", props.radius);
  props.border_width = number_field(lua, 1, "border_width", props.border_width);
  props.padding = number_field(lua, 1, "padding", props.padding);
  color_field(lua, 1, "color", props.color);
  props.border_color = optional_color(lua, 1, "border_color", Color{0.2F, 0.25F, 0.34F, 1.0F});
  props.text_color = optional_color(lua, 1, "text_color", Color{0.92F, 0.95F, 1.0F, 1.0F});
  props.placeholder_color =
      optional_color(lua, 1, "placeholder_color", Color{0.45F, 0.5F, 0.6F, 1.0F});
  props.font = string_field(lua, 1, "font", props.font);
  props.size = number_field(lua, 1, "size", props.size);
  props.weight = weight_field(lua, 1);
  props.password = boolean_field(lua, 1, "password", false);
  props.authenticate = boolean_field(lua, 1, "authenticate", false);
  props.autofocus = boolean_field(lua, 1, "autofocus", false);
  const lua_Integer max_length = integer_field(lua, 1, "max_length", 4096);
  if (max_length < 0) throw std::invalid_argument("text_field max_length cannot be negative");
  props.max_length = static_cast<std::size_t>(max_length);
  props.on_change = text_handler(function_field(lua, 1, "on_change"));
  // Lua uses on_submit for both: authenticate fields receive a boolean.
  FunctionRef submit = function_field(lua, 1, "on_submit");
  if (props.authenticate) props.on_authenticated = authentication_handler(std::move(submit));
  else props.on_submit = text_handler(std::move(submit));
  props.place = placement(lua, 1);
  return push_widget(lua, current_ui(lua).text_field(props));
}

int add_repeater(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  resolve_field(lua, 1, "items");
  luaL_checktype(lua, -1, LUA_TTABLE);
  const int items = lua_gettop(lua);
  lua_getfield(lua, 1, "render");
  luaL_checktype(lua, -1, LUA_TFUNCTION);
  const int render = lua_gettop(lua);
  lua_newtable(lua);
  const int result = lua_gettop(lua);
  lua_Integer output = 1;
  const lua_Unsigned count = lua_rawlen(lua, items);
  for (lua_Unsigned index = 1; index <= count; ++index) {
    lua_pushvalue(lua, render);
    lua_rawgeti(lua, items, static_cast<lua_Integer>(index));
    lua_pushinteger(lua, static_cast<lua_Integer>(index));
    if (lua_pcall(lua, 2, 1, 0) != LUA_OK) return lua_error(lua);
    if (!lua_isnil(lua, -1)) lua_rawseti(lua, result, output++);
    else lua_pop(lua, 1);
  }
  lua_pushvalue(lua, result);
  return 1;
}

int add_lazy(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  resolve_field(lua, 1, "visible");
  const bool visible = lua_isnil(lua, -1) || lua_toboolean(lua, -1) != 0;
  lua_pop(lua, 1);
  lua_getfield(lua, 1, "render");
  luaL_checktype(lua, -1, LUA_TFUNCTION);
  if (lua_pcall(lua, 0, 1, 0) != LUA_OK) return lua_error(lua);
  const Widget* widget = test_widget(lua, -1);
  if (widget == nullptr) throw std::invalid_argument("lazy render must return a widget");
  if (!visible) current_ui(lua).hide(*widget);
  return 1;
}

LayoutProps layout_props(lua_State* lua) {
  LayoutProps props;
  props.gap = number_field(lua, 1, "gap", 0.0F);
  props.padding = number_field(lua, 1, "padding", 0.0F);
  props.width = optional_number(lua, 1, "width");
  props.height = optional_number(lua, 1, "height");
  props.radius = number_field(lua, 1, "radius", 0.0F);
  props.place = placement(lua, 1);
  props.input = input(lua, 1);
  return props;
}

int add_linear(lua_State* lua) {
  const bool row = lua_toboolean(lua, lua_upvalueindex(2)) != 0;
  luaL_checktype(lua, 1, LUA_TTABLE);
  lua_getfield(lua, 1, "children");
  luaL_checktype(lua, -1, LUA_TTABLE);
  lua_pop(lua, 1);
  const std::vector<Widget> children = children_field(lua, 1, "children");
  const LayoutProps props = layout_props(lua);
  Ui& ui = current_ui(lua);
  return push_widget(lua, row ? ui.row(children, props) : ui.column(children, props));
}

int add_stack(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  const std::vector<Widget> children = children_field(lua, 1, "children");
  StackProps props;
  props.width = optional_number(lua, 1, "width");
  props.height = optional_number(lua, 1, "height");
  props.radius = number_field(lua, 1, "radius", 0.0F);
  props.place = placement(lua, 1);
  props.input = input(lua, 1);
  return push_widget(lua, current_ui(lua).stack(children, props));
}

ScrollProps scroll_props(lua_State* lua) {
  ScrollProps props;
  lua_getfield(lua, 1, "id");
  props.id = luaL_checkstring(lua, -1);
  lua_pop(lua, 1);
  props.width = number_field(lua, 1, "width", 0.0F);
  props.height = number_field(lua, 1, "height", 0.0F);
  props.gap = number_field(lua, 1, "gap", 0.0F);
  props.padding = number_field(lua, 1, "padding", 0.0F);
  props.scroll_step = number_field(lua, 1, "scroll_step", props.scroll_step);
  props.radius = number_field(lua, 1, "radius", 0.0F);
  props.place = placement(lua, 1);
  props.input = input(lua, 1);
  return props;
}

int add_scrollbox(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  const ScrollProps props = scroll_props(lua);
  const bool single = has_field(lua, 1, "content");
  const std::vector<Widget> children = children_field(lua, 1, single ? "content" : "children");
  return push_widget(lua, current_ui(lua).scrollbox(children, props));
}

int add_listview(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  const ScrollProps scroll = scroll_props(lua);
  ListviewProps props;
  props.id = scroll.id;
  props.width = scroll.width;
  props.height = scroll.height;
  props.gap = scroll.gap;
  props.scroll_step = scroll.scroll_step;
  props.radius = scroll.radius;
  props.place = scroll.place;
  props.input = scroll.input;
  props.item_height = number_field(lua, 1, "item_height", 0.0F);
  resolve_field(lua, 1, "items");
  luaL_checktype(lua, -1, LUA_TTABLE);
  const int items = lua_gettop(lua);
  lua_getfield(lua, 1, "render");
  luaL_checktype(lua, -1, LUA_TFUNCTION);
  const int render = lua_gettop(lua);
  props.count = static_cast<std::size_t>(lua_rawlen(lua, items));
  // Never raises a Lua error: this runs inside C++ frames.
  props.render = [lua, items, render](std::size_t index) -> std::optional<Widget> {
    lua_pushvalue(lua, render);
    lua_rawgeti(lua, items, static_cast<lua_Integer>(index + 1));
    lua_pushinteger(lua, static_cast<lua_Integer>(index + 1));
    if (lua_pcall(lua, 2, 1, 0) != LUA_OK) {
      std::string error = lua_tostring(lua, -1) == nullptr ? "listview render failed"
                                                           : lua_tostring(lua, -1);
      lua_pop(lua, 1);
      throw std::runtime_error(error);
    }
    const Widget* widget = test_widget(lua, -1);
    const bool skip = lua_isnil(lua, -1);
    std::optional<Widget> result;
    if (widget != nullptr) result = *widget;
    lua_pop(lua, 1);
    if (!result && !skip) throw std::invalid_argument("listview render must return a widget or nil");
    return result;
  };
  return push_widget(lua, current_ui(lua).listview(props));
}

int add_anchor_layout(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  AnchorLayoutProps props;
  props.width = number_field(lua, 1, "width", 0.0F);
  props.height = number_field(lua, 1, "height", 0.0F);
  props.radius = number_field(lua, 1, "radius", 0.0F);
  props.place = placement(lua, 1);
  props.input = input(lua, 1);
  lua_getfield(lua, 1, "children");
  luaL_checktype(lua, -1, LUA_TTABLE);
  const int children = lua_gettop(lua);
  const lua_Unsigned count = lua_rawlen(lua, children);
  for (lua_Unsigned index = 1; index <= count; ++index) {
    lua_rawgeti(lua, children, static_cast<lua_Integer>(index));
    luaL_checktype(lua, -1, LUA_TTABLE);
    const int entry = lua_gettop(lua);
    AnchorChild child;
    child.margin = margin_field(lua, entry);
    lua_getfield(lua, entry, "anchors");
    if (!lua_isnil(lua, -1)) {
      luaL_checktype(lua, -1, LUA_TTABLE);
      const int anchors = lua_gettop(lua);
      child.left = boolean_field(lua, anchors, "left", false);
      child.right = boolean_field(lua, anchors, "right", false);
      child.top = boolean_field(lua, anchors, "top", false);
      child.bottom = boolean_field(lua, anchors, "bottom", false);
    }
    lua_pop(lua, 1);
    child.width = optional_number(lua, entry, "width");
    child.height = optional_number(lua, entry, "height");
    child.min_width = number_field(lua, entry, "min_width", child.min_width);
    child.min_height = number_field(lua, entry, "min_height", child.min_height);
    child.max_width = number_field(lua, entry, "max_width", child.max_width);
    child.max_height = number_field(lua, entry, "max_height", child.max_height);
    lua_getfield(lua, entry, "render");
    luaL_checktype(lua, -1, LUA_TFUNCTION);
    // Replace the entry with its render function, which stays on the stack
    // at this index until the layout is built.
    lua_replace(lua, entry);
    const int render = entry;
    child.render = [lua, render](const AnchorBox& box) {
      lua_pushvalue(lua, render);
      lua_createtable(lua, 0, 4);
      set_number(lua, "width", box.width);
      set_number(lua, "height", box.height);
      set_number(lua, "x", box.x);
      set_number(lua, "y", box.y);
      if (lua_pcall(lua, 1, 1, 0) != LUA_OK) {
        std::string error = lua_tostring(lua, -1) == nullptr ? "anchor render failed"
                                                             : lua_tostring(lua, -1);
        lua_pop(lua, 1);
        throw std::runtime_error(error);
      }
      const Widget* widget = test_widget(lua, -1);
      if (widget == nullptr) {
        lua_pop(lua, 1);
        throw std::invalid_argument("anchor render must return a widget");
      }
      const Widget result = *widget;
      lua_pop(lua, 1);
      return result;
    };
    props.children.push_back(std::move(child));
  }
  return push_widget(lua, current_ui(lua).anchor_layout(props));
}

// --- surfaces and session --------------------------------------------------

int configure_toplevel(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  ToplevelProps props;
  props.width = optional_number(lua, 1, "width");
  props.height = optional_number(lua, 1, "height");
  lua_getfield(lua, 1, "refresh_interval");
  props.refresh_interval = lua_isnil(lua, -1) ? 1.0 : luaL_checknumber(lua, -1);
  lua_pop(lua, 1);
  props.title = string_field(lua, 1, "title", props.title);
  props.app_id = string_field(lua, 1, "app_id", props.app_id);
  current_ui(lua).toplevel(props);
  return 0;
}

int toplevel_dimensions(lua_State* lua) {
  const Size size = toplevel_size();
  lua_createtable(lua, 0, 2);
  lua_pushinteger(lua, size.width);
  lua_setfield(lua, -2, "width");
  lua_pushinteger(lua, size.height);
  lua_setfield(lua, -2, "height");
  return 1;
}

int toplevel_close(lua_State*) {
  request_toplevel_close();
  return 0;
}

int configure_surface(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  Ui& ui = current_ui(lua);
  SurfaceProps props;
  props.width = optional_number(lua, 1, "width");
  props.height = optional_number(lua, 1, "height");
  props.anchor = anchor_field(lua, 1).value_or(anchor::center);
  props.layer = layer_field(lua, 1);
  props.margin = margin_field(lua, 1);
  props.surface_namespace = string_field(lua, 1, "namespace", ui.scene().surface_namespace);
  props.focus_grab = boolean_field(lua, 1, "focus_grab", false);
  props.on_activated = focus_handler(function_field(lua, 1, "on_activated"));
  props.on_dismiss = focus_handler(function_field(lua, 1, "on_dismiss"));
  props.refresh_interval = optional_number(lua, 1, "refresh_interval").value_or(1.0);
  lua_getfield(lua, 1, "input_regions");
  if (!lua_isnil(lua, -1)) {
    luaL_checktype(lua, -1, LUA_TTABLE);
    const int regions = lua_gettop(lua);
    props.input_regions.emplace();
    const lua_Unsigned count = lua_rawlen(lua, regions);
    for (lua_Unsigned index = 1; index <= count; ++index) {
      lua_rawgeti(lua, regions, static_cast<lua_Integer>(index));
      luaL_checktype(lua, -1, LUA_TTABLE);
      std::array<std::int32_t, 4> region{};
      for (int field = 0; field < 4; ++field) {
        lua_geti(lua, -1, field + 1);
        region[static_cast<std::size_t>(field)] = static_cast<std::int32_t>(luaL_checkinteger(lua, -1));
        lua_pop(lua, 1);
      }
      props.input_regions->push_back(region);
      lua_pop(lua, 1);
    }
  }
  lua_pop(lua, 1);
  ui.surface(props);
  return 0;
}

int surface_focus_state(lua_State* lua) {
  const FocusGrabState state = focus_grab_state();
  lua_createtable(lua, 0, 2);
  lua_pushboolean(lua, state.available);
  lua_setfield(lua, -2, "available");
  lua_pushboolean(lua, state.active);
  lua_setfield(lua, -2, "active");
  return 1;
}

int configure_session_lock(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  // No on_unlock: a Lua lock screen exits once unlocked, as it always has.
  SessionLockProps props;
  props.pam_service = string_field(lua, 1, "pam_service");
  props.pam_conf = string_field(lua, 1, "pam_conf");
  current_ui(lua).session_lock(props);
  return 0;
}

int configure_lock_surface(lua_State* lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  current_ui(lua).lock_surface({optional_number(lua, 1, "width"), optional_number(lua, 1, "height")});
  return 0;
}

int session_surface(lua_State* lua) {
  const LockSurfaceContext context = lock_surface_context();
  lua_createtable(lua, 0, 3);
  lua_pushinteger(lua, context.width);
  lua_setfield(lua, -2, "width");
  lua_pushinteger(lua, context.height);
  lua_setfield(lua, -2, "height");
  lua_pushlstring(lua, context.output.data(), context.output.size());
  lua_setfield(lua, -2, "output");
  return 1;
}

int session_is_locked(lua_State* lua) {
  lua_pushboolean(lua, session_locked());
  return 1;
}

int session_failed(lua_State* lua) {
  lua_pushboolean(lua, session_authentication_failed());
  return 1;
}

int session_authentication(lua_State* lua) {
  std::size_t length = 0;
  const char* secret = luaL_checklstring(lua, 1, &length);
  lua_pushboolean(lua, session_authenticate(std::string_view(secret, length)));
  return 1;
}

// --- utilities -------------------------------------------------------------

int datetime_part(lua_State* lua) {
  const auto part = lua_tointeger(lua, lua_upvalueindex(1));
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  lua_pushinteger(lua, part == 0 ? local.tm_sec : part == 1 ? local.tm_min : local.tm_hour);
  return 1;
}

int datetime_format(lua_State* lua) {
  const char* format = luaL_optstring(lua, 1, "%H:%M:%S");
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  std::array<char, 256> result{};
  if (std::strftime(result.data(), result.size(), format, &local) == 0)
    throw std::length_error("datetime format result is too long");
  lua_pushstring(lua, result.data());
  return 1;
}

int datetime_monotonic(lua_State* lua) {
  static const auto started = std::chrono::steady_clock::now();
  lua_pushnumber(lua, std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
  return 1;
}

int datetime_unix(lua_State* lua) {
  lua_pushnumber(lua, std::chrono::duration<double>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  return 1;
}

int poll_command_call(lua_State* lua) {
  const std::string output = poll_command(luaL_checkstring(lua, 1), luaL_optnumber(lua, 2, 5.0));
  lua_pushlstring(lua, output.data(), output.size());
  return 1;
}

// vw.process.spawn("cmd") or vw.process.spawn({"argv0", "arg", ...})
int process_spawn(lua_State* lua) {
  if (lua_type(lua, 1) == LUA_TSTRING) {
    lua_pushinteger(lua, spawn_shell(lua_tostring(lua, 1)));
    return 1;
  }
  luaL_checktype(lua, 1, LUA_TTABLE);
  std::vector<std::string> arguments;
  const lua_Unsigned count = lua_rawlen(lua, 1);
  for (lua_Unsigned index = 1; index <= count; ++index) {
    lua_rawgeti(lua, 1, static_cast<lua_Integer>(index));
    if (lua_type(lua, -1) != LUA_TSTRING) throw std::invalid_argument("spawn arguments must be strings");
    arguments.emplace_back(lua_tostring(lua, -1));
    lua_pop(lua, 1);
  }
  lua_pushinteger(lua, spawn(arguments));
  return 1;
}

// --- installation ----------------------------------------------------------

// Registers fn with the state's Context as upvalue 1.
void set_context_function(lua_State* lua, int context_index, const char* name, lua_CFunction function) {
  lua_pushvalue(lua, context_index);
  lua_pushcclosure(lua, function, 1);
  lua_setfield(lua, -2, name);
}

// Makes every vw.<module> and vw.<module>.<submodule> table require()-able.
void register_modules(lua_State* lua, int table, const std::string& prefix, int loaded, int depth) {
  lua_pushnil(lua);
  while (lua_next(lua, table) != 0) {
    if (lua_type(lua, -2) == LUA_TSTRING && lua_istable(lua, -1)) {
      const std::string name = prefix + "." + lua_tostring(lua, -2);
      lua_pushvalue(lua, -1);
      lua_setfield(lua, loaded, name.c_str());
      if (depth > 0) register_modules(lua, lua_gettop(lua), name, loaded, depth - 1);
    }
    lua_pop(lua, 1);
  }
}

void install_api(lua_State* lua, int ctx) {
  luaL_newmetatable(lua, widget_type);
  lua_pop(lua, 1);
  lua_newtable(lua);  // vw
  lua_pushliteral(lua, VW_VERSION);
  lua_setfield(lua, -2, "version");

  lua_newtable(lua);  // vw.base
  set_context_function(lua, ctx, "rectangle", guarded<add_rectangle>);
  for (const char* name : {"hit_area", "hitArea", "drag"})
    set_context_function(lua, ctx, name, guarded<add_hit_area>);
  set_context_function(lua, ctx, "text", guarded<add_text>);
  set_context_function(lua, ctx, "image", guarded<add_image>);
  set_context_function(lua, ctx, "canvas", guarded<add_canvas>);
  set_context_function(lua, ctx, "text_field", guarded<add_text_field>);
  set_function(lua, "repeater", guarded<add_repeater>);
  set_context_function(lua, ctx, "lazy", guarded<add_lazy>);
  for (const bool row : {true, false}) {
    lua_pushvalue(lua, ctx);
    lua_pushboolean(lua, row);
    lua_pushcclosure(lua, guarded<add_linear>, 2);
    lua_setfield(lua, -2, row ? "row" : "column");
  }
  set_context_function(lua, ctx, "stack", guarded<add_stack>);
  set_context_function(lua, ctx, "scrollbox", guarded<add_scrollbox>);
  set_context_function(lua, ctx, "listview", guarded<add_listview>);
  set_context_function(lua, ctx, "anchor_layout", guarded<add_anchor_layout>);
  lua_setfield(lua, -2, "base");

  lua_newtable(lua);  // vw.toplevel
  set_context_function(lua, ctx, "set", guarded<configure_toplevel>);
  set_function(lua, "size", toplevel_dimensions);
  set_function(lua, "close", toplevel_close);
  lua_setfield(lua, -2, "toplevel");

  lua_newtable(lua);  // vw.surface
  set_context_function(lua, ctx, "set", guarded<configure_surface>);
  set_function(lua, "focus_state", surface_focus_state);
  lua_setfield(lua, -2, "surface");

  install_animation(lua);
  lua_setfield(lua, -2, "animation");

  lua_newtable(lua);  // vw.session
  set_context_function(lua, ctx, "lock", guarded<configure_session_lock>);
  set_context_function(lua, ctx, "locksurface", guarded<configure_lock_surface>);
  set_function(lua, "surface", session_surface);
  set_function(lua, "locked", session_is_locked);
  set_function(lua, "failed", session_failed);
  set_function(lua, "authenticate", guarded<session_authentication>);
  lua_setfield(lua, -2, "session");

  lua_newtable(lua);  // vw.datetime
  for (lua_Integer part = 0; part < 3; ++part) {
    lua_pushinteger(lua, part);
    lua_pushcclosure(lua, datetime_part, 1);
    lua_setfield(lua, -2, part == 0 ? "second" : part == 1 ? "minute" : "hour");
  }
  set_function(lua, "format", guarded<datetime_format>);
  set_function(lua, "monotonic", datetime_monotonic);
  set_function(lua, "unix", datetime_unix);
  lua_setfield(lua, -2, "datetime");

  lua_newtable(lua);  // vw.poll
  set_function(lua, "command", guarded<poll_command_call>);
  lua_setfield(lua, -2, "poll");

  lua_newtable(lua);  // vw.process
  set_function(lua, "spawn", guarded<process_spawn>);
  lua_setfield(lua, -2, "process");

  lua_newtable(lua);  // vw.services
  install_applications(lua);
  lua_setfield(lua, -2, "applications");
  install_hyprland(lua);
  lua_setfield(lua, -2, "hyprland");
  install_mpris(lua);
  lua_setfield(lua, -2, "mpris");
  install_zwwm(lua);
  lua_setfield(lua, -2, "zwwm");
  install_notifications(lua);
  lua_setfield(lua, -2, "notifications");
  install_pipewire(lua);
  lua_setfield(lua, -2, "pipewire");
  lua_setfield(lua, -2, "services");

  install_socket(lua);
  lua_setfield(lua, -2, "socket");
  install_json(lua);
  lua_setfield(lua, -2, "json");

  const int vw_table = lua_gettop(lua);
  lua_getglobal(lua, "package");
  lua_getfield(lua, -1, "loaded");
  const int loaded = lua_gettop(lua);
  lua_pushvalue(lua, vw_table);
  lua_setfield(lua, loaded, "vw");
  register_modules(lua, vw_table, "vw", loaded, 1);
  // Historical aliases: these resolve to functions rather than module tables.
  lua_getfield(lua, vw_table, "session");
  lua_getfield(lua, -1, "lock");
  lua_setfield(lua, loaded, "vw.session.lock");
  lua_getfield(lua, -1, "locksurface");
  lua_setfield(lua, loaded, "vw.session.locksurface");
  lua_pop(lua, 3);
  lua_setglobal(lua, "vw");
}

// Lets configurations require() modules that live next to them.
void prepend_package_path(lua_State* lua, const std::filesystem::path& config) {
  const std::filesystem::path directory = config.has_parent_path() ? config.parent_path() : ".";
  lua_getglobal(lua, "package");
  lua_getfield(lua, -1, "path");
  const std::string current = lua_isstring(lua, -1) ? lua_tostring(lua, -1) : "";
  lua_pop(lua, 1);
  const std::string base = directory.string();
  const std::string path = base + "/?.lua;" + base + "/?/init.lua;" + current;
  lua_pushlstring(lua, path.data(), path.size());
  lua_setfield(lua, -2, "path");
  lua_pop(lua, 1);
}

// Clears Context::ui however the build ends.
struct BuildScope {
  Context& context;
  ~BuildScope() { context.ui = nullptr; }
};

}  // namespace

Scene load_config(const std::filesystem::path& path) {
  begin_animation_frame();
  std::shared_ptr<lua_State> state(luaL_newstate(), &lua_close);
  if (!state) throw std::runtime_error("could not create Lua state");
  lua_State* lua = state.get();
  luaL_openlibs(lua);

  auto* ctx = new (lua_newuserdatauv(lua, sizeof(Context), 0)) Context{};
  if (luaL_newmetatable(lua, context_type) != 0) {
    lua_pushcfunction(lua, destroy_context);
    lua_setfield(lua, -2, "__gc");
  }
  lua_setmetatable(lua, -2);
  ctx->state = state;
  // Anchor the context in the registry for the lifetime of the state.
  lua_pushvalue(lua, -1);
  lua_setfield(lua, LUA_REGISTRYINDEX, context_key);
  const int ctx_index = lua_gettop(lua);

  install_api(lua, ctx_index);
  lua_pop(lua, 1);
  prepend_package_path(lua, path);

  Scene scene;
  Ui ui(scene);
  {
    ctx->ui = &ui;
    const BuildScope scope{*ctx};
    if (luaL_loadfile(lua, path.c_str()) != LUA_OK || lua_pcall(lua, 0, 0, 0) != LUA_OK) {
      const char* message = lua_tostring(lua, -1);
      throw std::runtime_error(message == nullptr ? "Lua configuration failed" : message);
    }
  }
  ui.finish();
  scene.animating = animations_active();
  return scene;
}

std::vector<SceneSource::EventSource> LuaSource::event_sources() {
  return {{poll_event_fd(), drain_poll_events},
          {hyprland::event_fd(), hyprland::drain_events},
          {zwwm::event_fd(), zwwm::drain_events},
          {notifications::event_fd(), notifications::drain_events},
          {pipewire::event_fd(), pipewire::drain_events}};
}

std::optional<std::filesystem::path> LuaSource::watch_directory() const {
  return path_.has_parent_path() ? path_.parent_path() : std::filesystem::path(".");
}

// The config itself, modules it may require(), and canvas scripts.
bool LuaSource::affects(std::string_view name) const {
  if (name == path_.filename().native()) return true;
  if (name.starts_with('.')) return false;
  return name.ends_with(".lua") || name.ends_with(".js");
}

}  // namespace vw
