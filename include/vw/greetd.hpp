#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A greetd client, for writing a greeter: greetd authenticates and starts the
// session, the greeter only asks questions and shows answers. greetd runs the
// greeter with $GREETD_SOCK set; replies arrive on event_fd(), so the loop
// stays responsive while PAM thinks.
//
//   app.on_event(vw::greetd::event_fd(), vw::greetd::drain_events);
//   vw::greetd::begin("alice");      // greetd asks: state().prompt == secret
//   vw::greetd::answer(password);    // until state().stage == authenticated
//   vw::greetd::start({"zwwm"});     // then exit once stage == started
namespace vw::greetd {

enum class Stage : std::uint8_t {
  idle,           // no login in progress
  waiting,        // a request is out; greetd has not answered yet
  prompting,      // greetd asks something: see prompt and message
  authenticated,  // every question answered: start() the session
  started,        // the session is starting; the greeter should exit
  failed,         // the login was refused or broke: message says why
};

enum class Prompt : std::uint8_t {
  none,
  visible,  // show the answer as it is typed (a username, a one-time code)
  secret,   // hide it (a password)
  info,     // only text to show; answer() to go on
  error,    // a PAM error to show; answer() to go on
};

struct State {
  Stage stage = Stage::idle;
  Prompt prompt = Prompt::none;
  // The question, the information, or why the login failed.
  std::string message;
  std::string username;
  // The last failure was wrong credentials, not an error in greetd.
  bool denied = false;
};

// $GREETD_SOCK is set and greetd answers.
bool available();
State state();

// Starts a login, cancelling one already in progress.
void begin(std::string_view username);
// Answers the current prompt; info and error prompts take no text.
void answer(std::string_view response = {});
// Starts the authenticated session: the command and extra KEY=value
// environment for it.
void start(const std::vector<std::string>& command, const std::vector<std::string>& environment = {});
// Abandons the login (to switch users); state() returns to idle.
void cancel();

// Readable when greetd replied; see vw::App::on_event. -1 without greetd.
int event_fd();
bool drain_events();

// People who can log in: /etc/passwd accounts with a uid in the login range
// (UID_MIN to UID_MAX from /etc/login.defs) and a real shell.
struct User {
  std::string name;
  std::string full_name;  // the GECOS name, or empty
  std::string home;
  std::uint32_t uid = 0;
};
std::vector<User> users();

// Installed sessions: Wayland ones from wayland-sessions, then X ones from
// xsessions, in $XDG_DATA_DIRS (or /usr/local/share and /usr/share).
struct Session {
  std::string name;
  std::vector<std::string> command;  // Exec, split into words
  std::string file;
  bool wayland = true;
};
std::vector<Session> sessions();

}  // namespace vw::greetd
