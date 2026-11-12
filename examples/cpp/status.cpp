// A status bar built from vw's system services: tray icons with their menus,
// network, Bluetooth, battery, power profile and screen brightness. Services
// that are missing on this machine simply leave their widget out.
//
//   cmake --build build && ./build/vw-example-status
#include <vw/text.hpp>
#include <vw/vw.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace {

constexpr float height = 34;
constexpr float menu_width = 240;
constexpr float menu_row = 30;
constexpr const char* font = "sans";
constexpr std::uint32_t right_button = 273;  // BTN_RIGHT

const vw::Color surface = vw::rgb(0x1e1e2e);
const vw::Color raised = vw::rgb(0x313244);
const vw::Color hover = vw::rgb(0x45475a);
const vw::Color text = vw::rgb(0xcdd6f4);
const vw::Color muted = vw::rgb(0x7f849c);
const vw::Color accent = vw::rgb(0x89b4fa);
const vw::Color warning = vw::rgb(0xf38ba8);

struct State {
  // Tray item whose menu is open, and the menu's right edge offset.
  std::string menu_item;
  float menu_right = 0;
};

State state;

// An icon by theme name or file; skipped when it cannot be loaded.
float icon(vw::Ui& ui, const std::string& name, const std::string& path, float x,
           vw::Input input = {}) {
  constexpr float size = 18;
  try {
    ui.image({.path = path, .icon = path.empty() ? name : "", .width = size, .height = size,
              .fit = vw::ImageFit::contain, .place = {.x = x, .y = (height - size) / 2},
              .input = std::move(input)});
  } catch (const std::exception&) {
    return 0;
  }
  return size;
}

// A label right-aligned at right; returns its left edge.
float label(vw::Ui& ui, const std::string& value, float right, vw::Color color = text) {
  const vw::Widget widget = ui.text({.text = value, .font = font, .size = 13, .color = color});
  ui.move(widget, right - widget.width, (height - widget.height) / 2);
  return right - widget.width;
}

std::string duration(std::int64_t seconds) {
  return std::format("{}:{:02}", seconds / 3600, seconds / 60 % 60);
}

void bar(vw::Ui& ui) {
  const vw::Size size = vw::surface_size();
  const float width = size.width > 0 ? static_cast<float>(size.width) : 1920.0F;
  // Scrolling anywhere on the bar changes the screen brightness.
  ui.rectangle({.width = width, .height = height, .color = surface,
                .input = {.on_scroll = [](const vw::ScrollEvent& event) {
                  vw::brightness::change_screen(event.delta > 0 ? -0.05 : 0.05);
                }}});

  const auto now = std::chrono::zoned_time{std::chrono::current_zone(),
                                           std::chrono::system_clock::now()};
  const vw::Widget clock = ui.text({.text = std::format("{:%a %d  %H:%M}", now), .font = font,
                                    .size = 13, .weight = vw::weight::semibold, .color = text});
  ui.move(clock, 14, (height - clock.height) / 2);

  // Right to left.
  float x = width - 14;

  if (const auto profile = vw::upower::profile(); !profile.empty()) {
    const float chip = vw::caret_stops(profile, font, 12, vw::weight::regular).back().x + 20;
    x -= chip;
    ui.rectangle({.width = chip, .height = 22, .radius = 11, .color = accent,
                  .place = {.x = x, .y = (height - 22) / 2},
                  .input = {.on_click = [](const vw::ClickEvent&) {
                              // Cycle through the daemon's profiles.
                              const auto all = vw::upower::profiles();
                              const auto current = std::ranges::find(all, vw::upower::profile());
                              if (all.empty()) return;
                              const auto next = current == all.end() || current + 1 == all.end()
                                                    ? all.begin() : current + 1;
                              vw::upower::set_profile(*next);
                            },
                            .hover = vw::Hover{.color = vw::rgb(0xb4befe)}}});
    const vw::Widget front = ui.text({.text = profile, .font = font, .size = 12,
                                      .color = surface});
    ui.move(front, x + 10, (height - front.height) / 2);
    x -= 14;
  }

  if (const auto battery = vw::upower::display_device()) {
    const bool low = battery->percentage <= 15 && !battery->charging();
    std::string value = std::format("{:.0f}%", battery->percentage);
    if (battery->state == vw::upower::State::discharging && battery->time_to_empty > 0)
      value += "  " + duration(battery->time_to_empty);
    x = label(ui, value, x, low ? warning : text) - 6;
    x -= icon(ui, battery->icon_name, "", x - 18) + 14;
  }

  if (const auto backlight = vw::brightness::screen()) {
    x = label(ui, std::format("{:.0f}%", backlight->fraction() * 100), x) - 6;
    x -= icon(ui, "display-brightness-symbolic", "", x - 18) + 14;
  }

  if (vw::bluetooth::available()) {
    const auto adapter = vw::bluetooth::adapter();
    int connected = 0;
    for (const auto& device : vw::bluetooth::devices()) connected += device.connected ? 1 : 0;
    if (connected > 0) x = label(ui, std::to_string(connected), x) - 4;
    const bool powered = adapter && adapter->powered;
    x -= icon(ui, powered ? "bluetooth-active-symbolic" : "bluetooth-disabled-symbolic", "",
              x - 18, {.on_click = [powered](const vw::ClickEvent&) {
                vw::bluetooth::set_powered(!powered);
              }}) + 14;
  }

  const auto network = vw::network::status();
  if (network.available) {
    const std::string name = network.primary == vw::network::Kind::wifi ? network.ssid
                             : network.connection.empty()              ? "Offline"
                                                                       : network.connection;
    x = label(ui, name, x, network.primary == vw::network::Kind::none ? muted : text) - 6;
    x -= icon(ui, vw::network::icon_name(network), "", x - 18) + 18;
  }

  // Tray: left click activates (or opens the menu of menu-only items), right
  // click opens the menu.
  const auto items = vw::tray::items();
  for (auto item = items.rbegin(); item != items.rend(); ++item) {
    if (item->status == vw::tray::Status::passive) continue;
    const float right = width - x;
    x -= icon(ui, item->icon, item->icon_path, x - 18,
              {.on_click = [id = item->id, menu = item->has_menu, only_menu = item->item_is_menu,
                            right](const vw::ClickEvent& click) {
                 if (menu && (click.button == right_button || only_menu)) {
                   state.menu_item = state.menu_item == id ? "" : id;
                   state.menu_right = right;
                 } else {
                   vw::tray::activate(id);
                 }
               },
               .hover = vw::Hover{.scale = 1.15F}}) + 10;
  }

  ui.surface({.width = 0, .height = height, .anchor = vw::anchor::top,
              .exclusive_zone = static_cast<std::int32_t>(height),
              .surface_namespace = "vw-status"});
}

float menu_height(const std::vector<vw::tray::MenuEntry>& entries) {
  float total = 0;
  for (const auto& entry : entries) {
    if (!entry.visible) continue;
    total += entry.separator ? 9 : menu_row + menu_height(entry.children);
  }
  return total;
}

// Lays out menu entries from y; returns the height used.
float menu_entries(vw::Ui& ui, const std::vector<vw::tray::MenuEntry>& entries, float y,
                   float indent) {
  const float start = y;
  for (const auto& entry : entries) {
    if (!entry.visible) continue;
    if (entry.separator) {
      ui.rectangle({.width = menu_width - 24, .height = 1, .color = raised,
                    .place = {.x = 12, .y = y + 4}});
      y += 9;
      continue;
    }
    const bool submenu = !entry.children.empty();
    ui.rectangle({.width = menu_width - 12, .height = menu_row, .radius = 8, .color = surface,
                  .place = {.x = 6, .y = y},
                  .input = {.on_click = [id = state.menu_item, entry = entry.id,
                                         enabled = entry.enabled && !submenu](const vw::ClickEvent&) {
                              if (!enabled) return;
                              vw::tray::activate_menu(id, entry);
                              state.menu_item.clear();
                            },
                            .hover = entry.enabled && !submenu
                                         ? std::optional(vw::Hover{.color = hover})
                                         : std::nullopt}});
    std::string caption = entry.label;
    if (entry.toggle != vw::tray::MenuEntry::Toggle::none)
      caption = std::string(entry.checked ? "● " : "○ ") + caption;
    const vw::Widget widget = ui.text({.text = caption, .font = font, .size = 13,
                                       .weight = submenu ? vw::weight::semibold : vw::weight::regular,
                                       .color = entry.enabled ? text : muted,
                                       .max_width = menu_width - 36 - indent, .max_lines = 1});
    ui.move(widget, 18 + indent, y + (menu_row - widget.height) / 2);
    y += menu_row;
    // Submenus are shown inline, indented.
    if (submenu) y += menu_entries(ui, entry.children, y, indent + 14);
  }
  return y - start;
}

void tray_menu(vw::Ui& ui) {
  if (state.menu_item.empty()) return;
  const auto entries = vw::tray::menu(state.menu_item);
  if (entries.empty()) {
    state.menu_item.clear();
    return;
  }
  const float used = menu_height(entries) + 12;
  ui.rectangle({.width = menu_width, .height = used, .radius = 14, .color = surface,
                .border_width = 1, .border_color = raised});
  menu_entries(ui, entries, 6, 0);

  ui.keyboard([](const vw::KeyEvent& key) {
    if (key.name != "Escape") return false;
    state.menu_item.clear();
    return true;
  }, vw::Keyboard::on_demand);
  // Centered under the icon (18 px wide, its right edge menu_right from the
  // screen edge).
  const float right = std::max(6.0F, state.menu_right + 9 - menu_width / 2);
  ui.surface({.width = menu_width, .height = used, .anchor = vw::anchor::top_right,
              .layer = vw::Layer::overlay,
              .margin = {6, static_cast<std::int32_t>(right), 0, 0},
              .surface_namespace = "vw-status-menu"});
}

}  // namespace

int main() {
  vw::tray::start();
  vw::App app;
  app.surface(bar).surface(tray_menu);
  app.on_event(vw::tray::event_fd(), vw::tray::drain_events)
      .on_event(vw::network::event_fd(), vw::network::drain_events)
      .on_event(vw::upower::event_fd(), vw::upower::drain_events)
      .on_event(vw::bluetooth::event_fd(), vw::bluetooth::drain_events)
      .on_event(vw::brightness::event_fd(), vw::brightness::drain_events);
  return app.run();
}
