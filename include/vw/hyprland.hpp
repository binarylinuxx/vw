#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Hyprland IPC. Requests time out after two seconds and throw on failure.
namespace vw::hyprland {

bool available();
// Raw request on the command socket, e.g. "j/clients" or "/dispatch workspace 2".
std::string request(std::string_view command);
// JSON query such as "workspaces", "activewindow" or "cursorpos".
nlohmann::json query(std::string_view endpoint);

struct CommandResult {
  bool ok = false;
  std::string response;
};

CommandResult dispatch(std::string_view arguments);
CommandResult keyword(std::string_view arguments);

struct Event {
  std::string name;
  std::string data;
  std::uint64_t sequence = 0;
};

// Most recent event from the event socket, once one has arrived.
std::optional<Event> last_event();
// Readable when new events arrive. Calling either function starts the event
// listener; pass both to vw::App::on_event to rebuild on Hyprland events.
int event_fd();
bool drain_events();

}  // namespace vw::hyprland
