// A dock of pinned applications along the bottom edge. Icons come from the
// desktop entries and the icon theme; a click launches the application.
//
//   cmake --build build && ./build/vw-example-dock
#include <vw/icon.hpp>
#include <vw/vw.hpp>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Pinned {
  std::string id;
  std::string icon;  // resolved file
};

// The best match for each query, without duplicates.
std::vector<Pinned> pinned() {
  std::vector<Pinned> result;
  for (const char* query : {"firefox", "terminal", "files", "editor", "settings", "music", "calculator"}) {
    const auto found = vw::applications::search(query, 1);
    if (found.empty()) continue;
    if (std::ranges::any_of(result, [&](const Pinned& item) { return item.id == found[0].id; })) continue;
    // Skip entries whose icon the theme does not have.
    const auto icon = vw::resolve_icon(found[0].icon, 48);
    if (icon.empty()) continue;
    result.push_back({found[0].id, icon.string()});
  }
  return result;
}

}  // namespace

int main() {
  const std::vector<Pinned> apps = pinned();
  vw::App app([&apps](vw::Ui& ui) {
    constexpr float cell = 60;
    constexpr float icon = 44;
    constexpr float pad = 8;
    const float width = cell * static_cast<float>(std::max<std::size_t>(apps.size(), 1)) + pad * 2;
    ui.rectangle({.width = width, .height = cell + pad * 2, .radius = 26,
                  .color = vw::rgba(0x151a1ee8), .border_width = 1,
                  .border_color = vw::rgba(0xffffff14)});
    for (std::size_t index = 0; index < apps.size(); ++index) {
      const float x = pad + cell * static_cast<float>(index);
      const std::string id = apps[index].id;
      const vw::Widget slot = ui.rectangle({
          .width = cell, .height = cell, .radius = 18, .color = vw::rgba(0xffffff00),
          .input = {.on_click = [id](const vw::ClickEvent&) {
                      try {
                        vw::applications::launch(id);
                      } catch (const std::exception& error) {
                        std::cerr << error.what() << '\n';
                      }
                    },
                    .hover = vw::Hover{.color = vw::rgba(0xffffff1c)}}});
      ui.move(slot, x, pad);
      const vw::Widget image = ui.image({.path = apps[index].icon, .width = icon, .height = icon,
                                         .fit = vw::ImageFit::contain});
      ui.move(image, x + (cell - image.width) / 2, pad + (cell - image.height) / 2);
    }
    // Nothing here changes on its own, so it only rebuilds on input.
    ui.surface({.anchor = vw::anchor::bottom, .margin = {0, 0, 12, 0},
                .surface_namespace = "vw-dock", .refresh_interval = 3600});
  });
  return app.run();
}
