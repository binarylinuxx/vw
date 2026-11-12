// A power menu over the whole screen: lock, log out, reboot or shut down.
// Arrows move the selection, Enter picks it, Escape or a click outside
// closes the menu.
//
//   cmake --build build && ./build/vw-example-power
#include <vw/icon.hpp>
#include <vw/vw.hpp>

#include <array>
#include <cstdlib>

namespace {

struct Action {
  const char* label;
  const char* icon;
  const char* command;
};

constexpr std::array<Action, 4> actions{{
    {"Lock", "system-lock-screen", "loginctl lock-session"},
    {"Log out", "system-log-out", "loginctl terminate-session \"$XDG_SESSION_ID\""},
    {"Reboot", "system-reboot", "systemctl reboot"},
    {"Shut down", "system-shutdown", "systemctl poweroff"},
}};

int selected = 0;

[[noreturn]] void run(std::size_t index) {
  vw::spawn_shell(actions[index].command);
  std::exit(EXIT_SUCCESS);
}

}  // namespace

int main() {
  vw::App app([](vw::Ui& ui) {
    const vw::Size size = vw::surface_size();
    const float width = size.width > 0 ? static_cast<float>(size.width) : 1920;
    const float height = size.height > 0 ? static_cast<float>(size.height) : 1080;

    // The dim backdrop doubles as the "click outside" target.
    ui.rectangle({.width = width, .height = height, .color = vw::rgba(0x0b0f11cc),
                  .input = {.on_click = [](const vw::ClickEvent&) { std::exit(EXIT_SUCCESS); }}});

    constexpr float button = 132;
    constexpr float gap = 20;
    const float row = button * actions.size() + gap * (actions.size() - 1);
    const float left = (width - row) / 2;
    const float top = (height - button) / 2;
    for (std::size_t index = 0; index < actions.size(); ++index) {
      const bool active = static_cast<int>(index) == selected;
      const float x = left + static_cast<float>(index) * (button + gap);
      const vw::Widget card = ui.rectangle({
          .width = button, .height = button, .radius = 32,
          .color = active ? vw::rgb(0xa7cbe7) : vw::rgb(0x20272c),
          .input = {.on_click = [index](const vw::ClickEvent&) { run(index); },
                    .hover = vw::Hover{.color = active ? vw::rgb(0xbde1fe) : vw::rgb(0x262d33)}}});
      ui.move(card, x, top);
      // Not every icon theme has these; the label is enough without one.
      if (const auto path = vw::resolve_icon(actions[index].icon, 48); !path.empty()) {
        const vw::Widget icon = ui.image({.path = path.string(), .width = 40, .height = 40,
                                          .fit = vw::ImageFit::contain});
        ui.move(icon, x + (button - icon.width) / 2, top + 30);
      }
      const vw::Widget label = ui.text({.text = actions[index].label, .font = "sans", .size = 15,
                                        .weight = vw::weight::medium,
                                        .color = active ? vw::rgb(0x1f445b) : vw::rgb(0xdfe6ee)});
      ui.move(label, x + (button - label.width) / 2, top + button - 30 - label.height / 2);
    }

    ui.keyboard(
        [](const vw::KeyEvent& key) {
          const int count = static_cast<int>(actions.size());
          if (key.name == "Escape") std::exit(EXIT_SUCCESS);
          else if (key.name == "Left") selected = (selected + count - 1) % count;
          else if (key.name == "Right" || key.name == "Tab") selected = (selected + 1) % count;
          else if (key.name == "Return") run(static_cast<std::size_t>(selected));
          else return false;
          return true;
        },
        vw::Keyboard::exclusive);
    ui.surface({.width = 0, .height = 0, .layer = vw::Layer::overlay,
                .surface_namespace = "vw-power", .refresh_interval = 3600});
  });
  return app.run();
}
