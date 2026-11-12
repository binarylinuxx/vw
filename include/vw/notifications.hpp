#pragma once

#include <cstdint>
#include <string>
#include <vector>

// An org.freedesktop.Notifications daemon (spec 1.2) on its own thread, and
// the model a shell draws from: what is open, what pops up, and what a
// notification centre keeps.
//
//   vw::notifications::start();
//   app.on_event(vw::notifications::event_fd(), vw::notifications::drain_events);
//   for (const auto& n : vw::notifications::popups()) ...
//
// Lifecycle of one notification:
//   Notify            open, pops up (unless do-not-disturb, which only
//                     critical ones get through)
//   hide_popup        stops popping up; still open and in the history
//   timeout           closed (reason 1); stays in the history
//   dismiss / action  closed (reason 2) and gone from the history
//   CloseNotification closed by the sender (reason 3) and gone
// Transient notifications never enter the history.
namespace vw::notifications {

struct Action {
  std::string key;
  std::string label;
};

struct Notification {
  std::uint32_t id = 0;
  std::string app_name;
  std::string app_icon;
  std::string summary;
  // May hold the spec's markup subset (b, i, u, a, img).
  std::string body;
  std::string image_path;
  std::string desktop_entry;
  // Ready-to-draw file paths, empty when there is nothing to show.
  // `icon` is the app's: app_icon as a path, URI or theme name, else the
  // desktop entry's icon. `image` is the notification's own (an avatar, a
  // cover): the image-data hint, else image-path. image-data is written to a
  // PNG that lives as long as the notification.
  std::string icon;
  std::string image;
  std::string category;
  std::vector<Action> actions;
  // 0 low, 1 normal, 2 critical
  std::uint8_t urgency = 1;
  // Progress 0-100 from the "value" hint; -1 without one.
  int value = -1;
  bool resident = false;
  bool transient = false;
  // Action keys are icon names ("action-icons" hint).
  bool action_icons = false;
  // Still to be shown as a popup.
  bool popup = false;
  bool closed = false;
  // Freedesktop reason: 1 expired, 2 dismissed, 3 closed by the sender.
  std::uint32_t close_reason = 0;
  // Unix seconds.
  std::int64_t timestamp = 0;
  // Milliseconds; 0 never expires.
  int expire_timeout = 5000;
};

struct StartResult {
  bool running = false;
  std::string error;
};

// Claims the bus name; blocks until it succeeds or fails.
StartResult start();
bool running();

// Newest first. Each is a cached snapshot rebuilt only after a change, so
// calling these on every frame is cheap. Theme icons are resolved on the
// calling thread: call them from the thread that draws.
// Open (not yet closed) notifications.
std::vector<Notification> list();
// What should be popping up now.
std::vector<Notification> popups();
// What a notification centre keeps: everything not transient that the user
// has not dismissed and the sender has not withdrawn; at most 256.
std::vector<Notification> history();
// Bumped on every change; equal values mean equal snapshots.
std::uint64_t generation();

// The user dismissed it: closes it (reason 2) and drops it from the history.
bool dismiss(std::uint32_t id);
// Stops it popping up, leaving it open and in the history.
void hide_popup(std::uint32_t id);
void hide_popups();
// Sends ActionInvoked; a notification that is not resident then closes as
// dismissed.
bool invoke(std::uint32_t id, const std::string& action_key);
// The default action: "default" if offered, else the only action.
bool activate(std::uint32_t id);
// Dismisses everything.
void clear();
// Enabling hides every popup; while on, only critical notifications pop up.
bool do_not_disturb();
void set_do_not_disturb(bool enabled);

// Readable when notifications change; see vw::App::on_event.
int event_fd();
bool drain_events();

}  // namespace vw::notifications
