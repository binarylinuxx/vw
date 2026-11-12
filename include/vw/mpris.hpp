#pragma once

#include <optional>
#include <string>
#include <vector>

// MPRIS media players on the session bus. State is kept fresh by a background
// thread from player signals; pass event_fd()/drain_events() to
// vw::App::on_event to rebuild on changes.
namespace vw::mpris {

struct Player {
  // Bus name, e.g. "org.mpris.MediaPlayer2.spotify", and its suffix.
  std::string service;
  std::string name;
  std::string title;
  std::string artist;
  std::string album;
  std::string art_url;
  std::string lyrics;
  std::string track_id;
  std::string playback_status = "Stopped";
  // Seconds; position is extrapolated while playing.
  double position = 0;
  double length = 0;
  bool can_go_previous = false;
  bool can_play = false;
  bool can_pause = false;
  bool can_go_next = false;
};

// The player to show: one that is playing (preferring one with artwork), else
// the previously active one, else the first.
std::optional<Player> active();
std::vector<Player> players();
bool available();

// Controls act on the active player and return false when it cannot.
bool previous();
bool play_pause();
bool next();
bool set_position(double seconds);

int event_fd();
bool drain_events();

}  // namespace vw::mpris
