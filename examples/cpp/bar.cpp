// A top bar written against libvw: a clock, the active Hyprland workspace,
// and a launcher button.
//
//   cmake --build build && ./build/vw-example-bar
#include <vw/vw.hpp>

#include <chrono>
#include <format>
#include <string>

int main() {
  constexpr float height = 36.0F;
  const vw::Color surface = vw::rgb(0x151a1e);
  const vw::Color text = vw::rgb(0xdfe6ee);
  const vw::Color accent = vw::rgb(0xa7cbe7);

  vw::App app([&](vw::Ui& ui) {
    // The bar fills the output width; until the compositor has said how wide
    // that is, lay out for a typical screen.
    const vw::Size size = vw::surface_size();
    const float width = size.width > 0 ? static_cast<float>(size.width) : 1920.0F;
    ui.rectangle({.width = width, .height = height, .color = surface});

    const auto now = std::chrono::zoned_time{std::chrono::current_zone(),
                                             std::chrono::system_clock::now()};
    const vw::Widget clock = ui.text({.text = std::format("{:%H:%M}", now), .size = 15,
                                      .color = text});
    ui.move(clock, (width - clock.width) / 2.0F, (height - clock.height) / 2.0F);

    std::string workspace = "-";
    if (vw::hyprland::available()) {
      try {
        workspace = vw::hyprland::query("activeworkspace").value("name", "-");
      } catch (const std::exception&) {
      }
    }
    const vw::Widget label = ui.text({.text = workspace, .size = 14, .color = accent});
    ui.move(label, 16.0F, (height - label.height) / 2.0F);

    // Widgets keep no state: the hover fade and the click both come from the
    // props, and the next rebuild picks up anything the handler changed.
    const vw::Widget button = ui.rectangle({
        .width = 28, .height = 28, .radius = 14, .color = accent,
        .input = {.on_click = [](const vw::ClickEvent&) { vw::spawn_shell("fuzzel"); },
                  .hover = vw::Hover{.color = vw::rgb(0xbde1fe)}}});
    ui.move(button, width - 40.0F, 4.0F);

    ui.surface({.width = 0, .height = height, .anchor = vw::anchor::top,
                .exclusive_zone = static_cast<std::int32_t>(height),
                .surface_namespace = "vw-bar"});
  });
  // Rebuild immediately on workspace changes instead of waiting for the
  // one-second refresh.
  app.on_event(vw::hyprland::event_fd(), vw::hyprland::drain_events);
  return app.run();
}
