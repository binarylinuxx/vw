// A desktop clock: greeting, time and date on a translucent card, below
// windows and click-through.
//
//   cmake --build build && ./build/vw-example-clock
#include <vw/vw.hpp>

#include <chrono>
#include <format>

int main() {
  vw::App app([](vw::Ui& ui) {
    const auto now = std::chrono::zoned_time{std::chrono::current_zone(),
                                             std::chrono::system_clock::now()};
    const auto local = now.get_local_time();
    const auto hour =
        std::chrono::hh_mm_ss{local - std::chrono::floor<std::chrono::days>(local)}.hours().count();
    const char* greeting = hour < 5    ? "Good night"
                           : hour < 12 ? "Good morning"
                           : hour < 18 ? "Good afternoon"
                                       : "Good evening";

    constexpr float width = 380;
    constexpr float height = 196;
    constexpr float inset = 28;
    ui.rectangle({.width = width, .height = height, .radius = 32, .color = vw::rgba(0x1e1e2ee0),
                  .border_width = 1, .border_color = vw::rgba(0xffffff14)});
    const vw::Widget hello = ui.text({.text = greeting, .font = "sans", .size = 15,
                                      .weight = vw::weight::medium, .color = vw::rgb(0x89b4fa)});
    ui.move(hello, inset, inset);
    const vw::Widget time = ui.text({.text = std::format("{:%H:%M}", now), .font = "sans",
                                     .size = 84, .weight = vw::weight::light,
                                     .color = vw::rgb(0xcdd6f4)});
    ui.move(time, inset - 4, inset + hello.height - 6);
    const vw::Widget date = ui.text({.text = std::format("{:%A, %d %B}", now), .font = "sans",
                                     .size = 17, .color = vw::rgb(0xa6adc8)});
    ui.move(date, inset, height - inset - date.height);

    // Below windows, never takes input, and only rebuilds once a second.
    ui.surface({.anchor = vw::anchor::top_left, .layer = vw::Layer::bottom,
                .margin = {48, 0, 0, 48}, .surface_namespace = "vw-clock",
                .input_regions = std::vector<std::array<std::int32_t, 4>>{},
                .refresh_interval = 1.0});
  });
  return app.run();
}
