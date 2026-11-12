// Two surfaces on one vw::App: a small top bar, and an application launcher
// popup that maps while open. Shows keyboard handling, text input with key
// repeat, scroll handlers and fitted icons.
//
//   cmake --build build && ./build/vw-example-launcher [--open] [--query TEXT]
#include <vw/vw.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace {

constexpr const char* query_id = "launcher-query";
constexpr std::size_t visible_rows = 8;

struct State {
  bool open = false;
  std::string query;
  std::size_t selected = 0;
  // The bar's scroll handler cycles this to show scroll events working.
  int accent = 0;
};

State state;

const vw::Color surface = vw::rgb(0x1e1e2e);
const vw::Color raised = vw::rgb(0x313244);
const vw::Color text = vw::rgb(0xcdd6f4);
const vw::Color muted = vw::rgb(0xa6adc8);
const vw::Color accents[] = {vw::rgb(0x89b4fa), vw::rgb(0xa6e3a1), vw::rgb(0xf5c2e7)};

void close_launcher() {
  state.open = false;
  state.query.clear();
  state.selected = 0;
  vw::reset_text_field(query_id);
}

void bar(vw::Ui& ui) {
  // Fills the output width (surface width 0); vw::surface_size() reports it.
  const vw::Size size = vw::surface_size();
  const float width = size.width > 0 ? static_cast<float>(size.width) : 1920.0F;
  constexpr float height = 36;
  const vw::Color accent = accents[state.accent];
  ui.rectangle({.width = width, .height = height, .color = surface,
                .input = {.on_scroll = [](const vw::ScrollEvent& event) {
                  const int step = event.delta > 0 ? 1 : 2;
                  state.accent = (state.accent + step) % 3;
                }}});
  const vw::Widget button = ui.rectangle({
      .width = 72, .height = 26, .radius = 13, .color = accent,
      .input = {.on_click = [](const vw::ClickEvent&) {
                  if (state.open) close_launcher();
                  else state.open = true;
                },
                .hover = vw::Hover{.color = text}}});
  ui.move(button, 8, 5);
  const vw::Widget label = ui.text({.text = "Apps", .size = 13, .weight = vw::weight::bold,
                                    .color = surface});
  ui.move(label, 8 + (72 - label.width) / 2, 5 + (26 - label.height) / 2);
  const vw::Widget hint = ui.text({.text = "scroll the bar to change its accent", .size = 12,
                                   .color = muted});
  ui.move(hint, width - hint.width - 12, (height - hint.height) / 2);
  ui.surface({.width = 0, .height = height, .anchor = vw::anchor::top,
              .exclusive_zone = static_cast<std::int32_t>(height), .surface_namespace = "vw-bar"});
}

void launcher(vw::Ui& ui) {
  if (!state.open) return;  // draws nothing: the surface unmaps

  constexpr float width = 520;
  constexpr float row = 48;
  constexpr float field = 48;
  const auto apps = vw::applications::search(state.query, visible_rows);
  if (state.selected >= apps.size()) state.selected = apps.empty() ? 0 : apps.size() - 1;
  const float height = 16 + field + 8 + row * static_cast<float>(std::max<std::size_t>(apps.size(), 1)) + 8;

  ui.rectangle({.width = width, .height = height, .radius = 20, .color = surface});
  ui.text_field({.id = query_id, .value = state.query, .placeholder = "Search applications",
                 .width = width - 16,
                 .height = field, .radius = 14, .border_width = 0, .padding = 16, .color = raised,
                 .text_color = text, .placeholder_color = muted, .size = 16, .autofocus = true,
                 .on_change = [](const std::string& value) {
                   state.query = value;
                   state.selected = 0;
                 },
                 .place = {.x = 8, .y = 8}});

  float y = 8 + field + 8;
  if (apps.empty()) {
    const vw::Widget none = ui.text({.text = "No matches", .size = 14, .color = muted});
    ui.move(none, (width - none.width) / 2, y + (row - none.height) / 2);
  }
  for (std::size_t index = 0; index < apps.size(); ++index) {
    const auto& app = apps[index];
    const bool selected = index == state.selected;
    ui.rectangle({.width = width - 16, .height = row, .radius = 12,
                  .color = selected ? raised : surface, .place = {.x = 8, .y = y},
                  .input = {.on_click = [id = app.id](const vw::ClickEvent&) {
                              vw::applications::launch(id);
                              close_launcher();
                            },
                            .hover = vw::Hover{.color = raised}}});
    if (!app.icon_path.empty()) {
      try {
        ui.image({.path = app.icon_path, .width = 28, .height = 28, .fit = vw::ImageFit::contain,
                  .place = {.x = 20, .y = y + 10}});
      } catch (const std::exception&) {
        // Undecodable icon; the row still works without it.
      }
    }
    const vw::Widget name = ui.text({.text = app.display_name, .size = 14,
                                     .weight = vw::weight::medium, .color = text,
                                     .max_width = width - 80, .max_lines = 1});
    ui.move(name, 60, y + (row - name.height) / 2);
    y += row;
  }

  ui.keyboard([apps](const vw::KeyEvent& key) {
    if (key.name == "Escape") {
      close_launcher();
    } else if (key.name == "Down" || (key.ctrl && key.name == "n")) {
      if (!apps.empty()) state.selected = (state.selected + 1) % apps.size();
    } else if (key.name == "Up" || (key.ctrl && key.name == "p")) {
      if (!apps.empty()) state.selected = (state.selected + apps.size() - 1) % apps.size();
    } else if (key.name == "Return" || key.name == "KP_Enter") {
      if (!apps.empty()) vw::applications::launch(apps[state.selected].id);
      close_launcher();
    } else {
      return false;  // typing goes to the search field
    }
    return true;
  }, vw::Keyboard::exclusive);

  ui.surface({.width = width, .height = height, .anchor = vw::anchor::center,
              .layer = vw::Layer::overlay, .surface_namespace = "vw-launcher"});
}

}  // namespace

int main(int argc, char** argv) {
  // --open starts with the launcher shown and --query TEXT pre-fills the
  // search, e.g. for screenshots.
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--open") state.open = true;
    else if (argument == "--query" && index + 1 < argc) state.query = argv[++index];
  }
  vw::App app;
  app.surface(bar).surface(launcher);
  return app.run();
}
