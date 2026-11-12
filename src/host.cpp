#include "vw/host.hpp"
#include "vw/auth.hpp"

#include <atomic>
#include <mutex>
#include <utility>

namespace vw {
namespace {

struct ToplevelState {
  Size size{800, 600};
  bool configured = false;
};

struct LockState {
  LockSurfaceContext context;
  std::string pam_service;
  std::string pam_conf;
};

std::mutex& host_mutex() {
  static std::mutex mutex;
  return mutex;
}

ToplevelState& toplevel() {
  static ToplevelState state;
  return state;
}

LockState& lock() {
  static LockState state;
  return state;
}

std::atomic_bool close_requested = false;
std::atomic_bool locked = false;
std::atomic_bool authenticated = false;
std::atomic_bool authentication_failed = false;
std::atomic_bool grab_available = false;
std::atomic_bool grab_active = false;

}  // namespace

Size toplevel_size() {
  std::lock_guard guard(host_mutex());
  return toplevel().size;
}

void request_toplevel_size(std::uint32_t width, std::uint32_t height) {
  std::lock_guard guard(host_mutex());
  if (!toplevel().configured) toplevel().size = {width, height};
}

void set_toplevel_size(std::uint32_t width, std::uint32_t height) {
  std::lock_guard guard(host_mutex());
  auto& state = toplevel();
  if (width != 0) state.size.width = width;
  if (height != 0) state.size.height = height;
  state.configured = true;
}

void request_toplevel_close() { close_requested.store(true); }

// Builds run on the event-loop thread, one surface at a time.
namespace {
Size current_surface_size;
}  // namespace
Size surface_size() { return current_surface_size; }
void set_surface_size(Size size) { current_surface_size = size; }
bool toplevel_close_requested() { return close_requested.load(); }

LockSurfaceContext lock_surface_context() {
  std::lock_guard guard(host_mutex());
  return lock().context;
}

void set_lock_surface_context(std::uint32_t width, std::uint32_t height,
                              const std::string& output) {
  std::lock_guard guard(host_mutex());
  lock().context = {width, height, output};
}

void configure_session_authentication(std::string service, std::string pam_conf) {
  std::lock_guard guard(host_mutex());
  lock().pam_service = std::move(service);
  lock().pam_conf = std::move(pam_conf);
}

void set_session_locked(bool value) { locked.store(value); }

void reset_session_authentication() {
  authenticated.store(false);
  authentication_failed.store(false);
}
bool session_locked() { return locked.load(); }
bool session_authentication_failed() { return authentication_failed.load(); }
bool session_unlock_requested() { return authenticated.load(); }

bool session_authenticate(std::string_view password) {
  std::string service;
  std::string conf;
  {
    std::lock_guard guard(host_mutex());
    service = lock().pam_service;
    conf = lock().pam_conf;
  }
  if (service.empty()) return false;
  const bool success = authenticate_password(password, service, conf);
  authentication_failed.store(!success);
  if (success) authenticated.store(true);
  return success;
}

FocusGrabState focus_grab_state() { return {grab_available.load(), grab_active.load()}; }

void set_focus_grab_state(bool available, bool active) {
  grab_available.store(available);
  grab_active.store(active);
}

}  // namespace vw
