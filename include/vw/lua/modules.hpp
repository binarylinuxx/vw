#pragma once

struct lua_State;

// Deprecated Lua frontend: each function pushes one vw.* module table.
namespace vw {

void install_animation(lua_State* lua);
void install_applications(lua_State* lua);
void install_hyprland(lua_State* lua);
void install_mpris(lua_State* lua);
void install_notifications(lua_State* lua);
void install_pipewire(lua_State* lua);
void install_socket(lua_State* lua);
void install_zwwm(lua_State* lua);

}  // namespace vw
