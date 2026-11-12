#include <vw/vw.hpp>

int main() {
  vw::App app([](vw::Ui& ui) {
    ui.rectangle({.width = 240, .height = 48, .radius = 12, .color = vw::rgb(0x1e1e2e)});
    ui.surface({.width = 240, .height = 48, .anchor = vw::anchor::top});
  });
  return app.run();
}
