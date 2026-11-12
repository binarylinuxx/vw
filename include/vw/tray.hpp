#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The system tray: StatusNotifierItem (KDE/AppIndicator) icons and their
// dbusmenu menus on the session bus. vw becomes the
// org.kde.StatusNotifierWatcher when no other is running, and is a tray host
// either way, so it works alongside another bar.
//
//   app.on_event(vw::tray::event_fd(), vw::tray::drain_events);
//   for (const auto& item : vw::tray::items())
//     ui.image({.path = item.icon_path, .icon = item.icon, .width = 18, .height = 18,
//               .input = {.on_click = [id = item.id](const vw::ClickEvent& click) {
//                 vw::tray::activate(id, click.x, click.y); }}});
namespace vw::tray {

enum class Status { passive, active, needs_attention };

struct Item {
  // Identifies the item for the calls below: "<bus name><object path>".
  std::string id;
  // The application's own id, e.g. "nm-applet", and its human title.
  std::string name;
  std::string title;
  // ApplicationStatus, Communications, SystemServices or Hardware.
  std::string category;
  Status status = Status::active;
  // Draw icon_path when set (an image file: an icon from the item's own
  // theme directory, or its pixmap written out as a cached PNG); otherwise
  // icon is a theme icon name for ImageProps::icon.
  std::string icon_path;
  std::string icon;
  std::string tooltip_title;
  // May contain simple markup.
  std::string tooltip_body;
  // The item only has a menu: open it on primary click instead of activate.
  bool item_is_menu = false;
  bool has_menu = false;
};

struct MenuEntry {
  std::int32_t id = 0;
  // Mnemonic underscores are removed.
  std::string label;
  std::string icon;
  bool enabled = true;
  bool visible = true;
  bool separator = false;
  enum class Toggle { none, checkmark, radio } toggle = Toggle::none;
  bool checked = false;
  std::vector<MenuEntry> children;
};

// Starts the host (and watcher) on first use; also started by the calls below.
void start();
bool available();
// In registration order. Passive items are included; hide them if wanted.
std::vector<Item> items();

// x and y are screen coordinates for the item's own popups, where known.
bool activate(const std::string& id, int x = 0, int y = 0);
bool secondary_activate(const std::string& id, int x = 0, int y = 0);
// The item draws its own menu; rarely works on Wayland, prefer menu().
bool context_menu(const std::string& id, int x = 0, int y = 0);
// delta in wheel steps; horizontal for sideways scrolling.
bool scroll(const std::string& id, int delta, bool horizontal = false);

// The item's menu tree (the root's children); cached until the item reports
// a change, so it is cheap to call while building.
std::vector<MenuEntry> menu(const std::string& id);
bool activate_menu(const std::string& id, std::int32_t entry);

int event_fd();
bool drain_events();

}  // namespace vw::tray
