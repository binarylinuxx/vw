#include "vw/app.hpp"
#include "vw/animation.hpp"
#include "vw/process.hpp"
#include "vw/renderer.hpp"
#include "vw/wayland.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace vw {

void run(SceneSource& source) {
  Renderer renderer;
  SceneSource* const surfaces[] = {&source};
  run_surfaces(source, surfaces, renderer);
}

class App::Surface : public SceneSource {
 public:
  explicit Surface(BuildFunction build) : build_(std::move(build)) {
    if (!build_) throw std::invalid_argument("vw::App surfaces require a build function");
  }

  Scene build() override {
    begin_animation_frame();
    Scene scene;
    Ui ui(scene);
    build_(ui);
    if (scene.rectangles.empty() && !scene.toplevel && !scene.session_lock) {
      scene.mapped = false;
      (void)animations_active();
      return scene;
    }
    ui.finish();
    scene.animating = animations_active();
    return scene;
  }

 private:
  BuildFunction build_;
};

App::App() { on_event(poll_event_fd(), drain_poll_events); }

App::App(BuildFunction build) : App() { surface(std::move(build)); }

App::~App() = default;

App& App::surface(BuildFunction build) {
  surfaces_.push_back(std::make_unique<Surface>(std::move(build)));
  return *this;
}

App& App::on_event(int fd, std::function<bool()> drain) {
  if (!drain) throw std::invalid_argument("event source needs a drain function");
  // Services report -1 when they are unavailable; there is nothing to watch.
  if (fd < 0) return *this;
  events_.push_back({fd, std::move(drain)});
  return *this;
}

Scene App::build() {
  if (surfaces_.empty()) throw std::logic_error("vw::App has no surfaces");
  return surfaces_.front()->build();
}

std::vector<SceneSource::EventSource> App::event_sources() { return events_; }

int App::run() {
  try {
    if (surfaces_.empty()) throw std::logic_error("vw::App has no surfaces");
    std::vector<SceneSource*> surfaces;
    for (const auto& surface : surfaces_) surfaces.push_back(surface.get());
    Renderer renderer;
    run_surfaces(*this, surfaces, renderer);
  } catch (const std::exception& error) {
    std::cerr << "vw: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

}  // namespace vw
