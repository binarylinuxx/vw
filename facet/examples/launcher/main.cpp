// Runs launcher.facet. Type to search, click or Enter to open, Escape quits.
#include <vw/applications.hpp>
#include <vw/vw.hpp>

#include <cstdlib>
#include <string>
#include <vector>

#include "launcher.facet.hpp"

struct App {
  std::string id;
  std::string name;
  std::string icon;
};

struct State {
  std::string query;
  std::vector<App> apps;

  void refresh() {
    apps.clear();
    for (const auto& found : vw::applications::search(query, 12))
      apps.push_back({found.id, found.name, found.icon_path});
  }
};

struct Actions {
  State* state;

  void search(const std::string& text) {
    state->query = text;
    state->refresh();
  }
  void open(const std::string& id) {
    vw::applications::launch(id);
    std::exit(EXIT_SUCCESS);
  }
  void open_first(const std::string&) {
    if (!state->apps.empty()) open(state->apps.front().id);
  }
};

int main() {
  State state;
  state.refresh();
  Actions actions{&state};
  vw::App app([&](vw::Ui& ui) {
    ui.keyboard([](const vw::KeyEvent& key) {
      if (key.name != "Escape") return false;
      std::exit(EXIT_SUCCESS);
    }, vw::Keyboard::exclusive);
    views::Launcher(ui, state, actions);
  });
  return app.run();
}
