// A month calendar in the top-right corner. The arrows (or the mouse wheel)
// page through months; today is highlighted.
//
//   cmake --build build && ./build/vw-example-calendar
#include <vw/vw.hpp>

#include <chrono>
#include <format>
#include <string>

namespace {

using namespace std::chrono;

int month_offset = 0;

const vw::Color surface = vw::rgb(0x151a1e);
const vw::Color text = vw::rgb(0xdfe6ee);
const vw::Color muted = vw::rgb(0x6f767d);
const vw::Color accent = vw::rgb(0xa7cbe7);

year_month_day today() {
  return year_month_day{floor<days>(zoned_time{current_zone(), system_clock::now()}.get_local_time())};
}

vw::Widget arrow(vw::Ui& ui, const char* glyph, float x, float y, int step) {
  const vw::Widget body = ui.rectangle({
      .width = 32, .height = 32, .radius = 16, .color = surface,
      .input = {.on_click = [step](const vw::ClickEvent&) { month_offset += step; },
                .hover = vw::Hover{.color = vw::rgb(0x20272c)}}});
  ui.move(body, x, y);
  const vw::Widget label = ui.text({.text = glyph, .font = "sans", .size = 16, .color = text});
  ui.move(label, x + (32 - label.width) / 2, y + (32 - label.height) / 2);
  return body;
}

}  // namespace

int main() {
  vw::App app([](vw::Ui& ui) {
    constexpr float cell = 40;
    constexpr float pad = 20;
    constexpr float width = cell * 7 + pad * 2;
    constexpr float grid_top = pad + 32 + 16 + 24;
    constexpr float height = grid_top + cell * 6 + pad;

    const year_month_day now = today();
    const year_month shown = year_month{now.year(), now.month()} + months{month_offset};

    ui.rectangle({.width = width, .height = height, .radius = 28, .color = surface,
                  .border_width = 1, .border_color = vw::rgba(0xffffff14),
                  .input = {.on_scroll = [](const vw::ScrollEvent& scroll) {
                    month_offset += scroll.delta > 0 ? 1 : -1;
                  }}});

    // Header: month and year, with the arrows on the right.
    const vw::Widget title = ui.text({.text = std::format("{:%B %Y}", shown), .font = "sans",
                                      .size = 18, .weight = vw::weight::semibold, .color = text});
    ui.move(title, pad + 4, pad + (32 - title.height) / 2);
    arrow(ui, "‹", width - pad - 68, pad, -1);
    arrow(ui, "›", width - pad - 32, pad, 1);

    // Weekdays, Monday first.
    for (unsigned index = 0; index < 7; ++index) {
      const vw::Widget day = ui.text({.text = std::format("{:%a}", weekday{(index + 1) % 7}).substr(0, 2),
                                      .font = "sans", .size = 12, .weight = vw::weight::medium,
                                      .color = muted});
      ui.move(day, pad + cell * static_cast<float>(index) + (cell - day.width) / 2,
              pad + 32 + 16);
    }

    // Day grid: the column of the 1st, then each day in turn.
    const unsigned first = (weekday{sys_days{shown / 1}}.iso_encoding() - 1);
    const unsigned last = static_cast<unsigned>((shown / std::chrono::last).day());
    for (unsigned day = 1; day <= last; ++day) {
      const unsigned slot = first + day - 1;
      const float x = pad + cell * static_cast<float>(slot % 7);
      const float y = grid_top + cell * static_cast<float>(slot / 7);
      const bool is_today = shown == year_month{now.year(), now.month()} &&
                            static_cast<unsigned>(now.day()) == day;
      if (is_today) {
        const vw::Widget mark = ui.rectangle({.width = cell - 4, .height = cell - 4,
                                              .radius = (cell - 4) / 2, .color = accent});
        ui.move(mark, x + 2, y + 2);
      }
      const vw::Widget number = ui.text({.text = std::to_string(day), .font = "sans", .size = 14,
                                         .weight = is_today ? vw::weight::bold : vw::weight::regular,
                                         .color = is_today ? surface : text});
      ui.move(number, x + (cell - number.width) / 2, y + (cell - number.height) / 2);
    }

    ui.surface({.anchor = vw::anchor::top_right, .margin = {12, 12, 0, 0},
                .surface_namespace = "vw-calendar", .refresh_interval = 60});
  });
  return app.run();
}
