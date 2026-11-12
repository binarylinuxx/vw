#include "vw/app.hpp"
#include "vw/animation.hpp"
#include "vw/process.hpp"
#include "vw/renderer.hpp"
#include "vw/wayland.hpp"

#include <algorithm>
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
  // `output` names the output a layer surface goes on unless it names one.
  explicit Surface(BuildFunction build, std::function<std::string()> output = {})
      : build_(std::move(build)), output_(std::move(output)) {
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
    if (output_ && scene.output.empty() && !scene.toplevel && !scene.session_lock) scene.output = output_();
    scene.animating = animations_active();
    return scene;
  }

 private:
  BuildFunction build_;
  std::function<std::string()> output_;
};

// Which output each per_output surface is for. A monitor keeps its slot while
// it stays connected, so plugging one in or out leaves the others' surfaces be.
class App::Outputs {
 public:
  explicit Outputs(std::size_t slots) : names_(slots) {}

  std::optional<Output> at(std::size_t slot) {
    const std::vector<Output> current = outputs();
    const auto connected = [&](const std::string& name) {
      return std::ranges::any_of(current, [&](const Output& output) { return output.name == name; });
    };
    for (std::string& name : names_)
      if (!name.empty() && !connected(name)) name.clear();
    for (const Output& output : current) {
      if (std::ranges::find(names_, output.name) != names_.end()) continue;
      const auto free = std::ranges::find(names_, std::string());
      if (free != names_.end()) *free = output.name;
    }
    for (const Output& output : current)
      if (output.name == names_[slot]) return output;
    return std::nullopt;
  }

  std::string name(std::size_t slot) const { return names_[slot]; }

 private:
  std::vector<std::string> names_;
};

App::App() { on_event(poll_event_fd(), drain_poll_events); }

App::App(BuildFunction build) : App() { surface(std::move(build)); }

App::~App() = default;

App& App::surface(BuildFunction build) {
  surfaces_.push_back(std::make_unique<Surface>(std::move(build)));
  return *this;
}

App& App::per_output(OutputBuildFunction build, std::size_t most) {
  if (!build) throw std::invalid_argument("vw::App surfaces require a build function");
  if (most == 0) throw std::invalid_argument("per_output needs room for at least one output");
  auto group = std::make_shared<Outputs>(most);
  output_groups_.push_back(group);
  auto shared = std::make_shared<OutputBuildFunction>(std::move(build));
  for (std::size_t slot = 0; slot < most; ++slot) {
    surfaces_.push_back(std::make_unique<Surface>(
        [group, shared, slot](Ui& ui) {
          // No monitor for this slot: it draws nothing and stays unmapped.
          if (const std::optional<Output> output = group->at(slot)) (*shared)(ui, *output);
        },
        [group, slot] { return group->name(slot); }));
  }
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
    // Monitors coming and going rebuild every surface (see vw::outputs()).
    if (!watching_outputs_) {
      watching_outputs_ = true;
      on_event(output_event_fd(), drain_output_events);
    }
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
