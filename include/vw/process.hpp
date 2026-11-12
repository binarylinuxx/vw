#pragma once

#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace vw {

// Starts a detached process in its own session with stdin on /dev/null and a
// clean signal mask, and returns its pid without waiting for it.
pid_t spawn(const std::vector<std::string>& argv);
// spawn({"/bin/sh", "-c", command}).
pid_t spawn_shell(std::string_view command);

// Returns the trimmed stdout of a shell command. The first call runs
// synchronously; later calls return the cached output and refresh it on a
// worker thread once `interval_seconds` has elapsed. A changed result signals
// poll_event_fd(), which vw::App watches to rebuild the scene.
std::string poll_command(const std::string& command, double interval_seconds = 5.0);
int poll_event_fd();
bool drain_poll_events();

}  // namespace vw
