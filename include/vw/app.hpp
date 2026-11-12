#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "vw/scene.hpp"
#include "vw/ui.hpp"

namespace vw {

// Anything that can rebuild a scene on demand. The Wayland loop rebuilds
// after input, on its refresh interval, and whenever an event source fires.
class SceneSource {
 public:
  struct EventSource {
    int fd = -1;
    // Drains the fd; returns true when the scene should be rebuilt.
    std::function<bool()> drain;
  };

  virtual ~SceneSource() = default;
  virtual Scene build() = 0;
  virtual std::vector<EventSource> event_sources() { return {}; }
  // Directory to watch for edits, and which changed file names matter.
  virtual std::optional<std::filesystem::path> watch_directory() const { return std::nullopt; }
  virtual bool affects(std::string_view /*file_name*/) const { return false; }
};

// Connects to the compositor, renders and runs the event loop until the
// surface closes or SIGINT/SIGTERM arrives. Throws on fatal errors.
void run(SceneSource& source);

// Surfaces built by C++ functions, all driven by one event loop.
//
//   int main() {
//     vw::App app;
//     app.surface([](vw::Ui& ui) { /* bar */ })
//        .surface([](vw::Ui& ui) { if (state.launcher_open) /* launcher */; });
//     app.on_event(vw::hyprland::event_fd(), vw::hyprland::drain_events);
//     return app.run();
//   }
//
// Every surface rebuilds after input on any of them and when an event source
// fires; each also rebuilds on its own refresh_interval (SurfaceProps,
// ToplevelProps), so a static surface can opt out of periodic work. A build that draws nothing leaves its
// surface unmapped until a later build draws something again, which is how
// popups, launchers, lock screens and windows come and go.
class App : public SceneSource {
 public:
  using BuildFunction = std::function<void(Ui&)>;

  App();
  // A single-surface app.
  explicit App(BuildFunction build);
  ~App() override;

  App& surface(BuildFunction build);

  // Rebuild whenever fd becomes readable and drain() returns true. A
  // negative fd (an unavailable service) is ignored.
  App& on_event(int fd, std::function<bool()> drain);

  // Builds the first surface; useful for checking a scene without Wayland.
  Scene build() override;
  std::vector<EventSource> event_sources() override;

  // Runs until closed. Returns EXIT_SUCCESS, or prints the error and returns
  // EXIT_FAILURE.
  int run();

 private:
  class Surface;
  std::vector<std::unique_ptr<Surface>> surfaces_;
  std::vector<EventSource> events_;
};

}  // namespace vw
