#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// State shared between the Wayland frontend and whatever builds the scene:
// compositor-provided sizes, lock-screen status and focus-grab status.
namespace vw {

struct Size {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

// Toplevel window size: the compositor's configure wins over the size the
// scene requested.
Size toplevel_size();
void request_toplevel_size(std::uint32_t width, std::uint32_t height);
void set_toplevel_size(std::uint32_t width, std::uint32_t height);
void request_toplevel_close();
// Whether the compositor draws this window's titlebar and borders, so the
// window should not draw its own (see ToplevelProps::decorations).
bool toplevel_server_decorated();
void set_toplevel_server_decorated(bool server);

// Size the compositor configured for the surface currently being built;
// {0, 0} until its first configure. Fill-size surfaces (SurfaceProps width or
// height 0) lay out against this.
Size surface_size();
// Set by the Wayland frontend before each build.
void set_surface_size(Size size);
bool toplevel_close_requested();

struct LockSurfaceContext {
  std::uint32_t width = 1280;
  std::uint32_t height = 720;
  std::string output;
};

// The output currently being rendered by a session-lock scene.
LockSurfaceContext lock_surface_context();
void set_lock_surface_context(std::uint32_t width, std::uint32_t height,
                              const std::string& output);

void configure_session_authentication(std::string service, std::string pam_conf);
void set_session_locked(bool locked);
// Clears the previous lock's authentication result; called when a new
// session lock starts.
void reset_session_authentication();
bool session_locked();
bool session_authentication_failed();
bool session_unlock_requested();
// Checks the password against PAM; success unlocks the session.
bool session_authenticate(std::string_view password);

// The system clipboard, through the surface that last received input.
// set_clipboard does nothing before any input has arrived (Wayland requires an
// input serial); clipboard_text returns nullopt when nothing text-like is on
// it. Text fields use it for Ctrl+C/X/V.
void set_clipboard(std::string_view text);
std::optional<std::string> clipboard_text();

struct FocusGrabState {
  bool available = false;
  bool active = false;
};

FocusGrabState focus_grab_state();
void set_focus_grab_state(bool available, bool active);

}  // namespace vw
