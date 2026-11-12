---
title: Apps and tray
description: Desktop entries for launchers, and StatusNotifierItems for a tray with menus.
---

## Applications — `vw/applications.hpp`

```cpp
for (const auto& app : vw::applications::search(query, 40))
  row(app.display_name, app.icon_path);
vw::applications::launch(app.id);
```

`search(query, limit)` returns visible applications, best match first — exact name, name prefix, word prefix, name substring, then other fields and keywords; an empty query lists everything by display name, and a limit of 0 is unlimited. `Application` carries `id`, `name`, `display_name`, `generic_name`, `description`, `executable`, `commandline`, `icon`, `icon_path` (resolved), `terminal`, `categories`, `keywords` and `startup_wm_class`. `launch(id)` starts it by desktop id (`firefox.desktop`) and throws on failure.

## Tray — `vw/tray.hpp`

`start()` provides the `org.kde.StatusNotifierWatcher` and hosts the items; then:

| Function | Meaning |
| --- | --- |
| `items()` | `id`, `title`, `status` (`passive`, `active`, `needs_attention`), `icon_path`/`icon`, tooltip, `has_menu` |
| `activate(id, x, y)`, `secondary_activate(…)` | left and middle click |
| `context_menu(id, x, y)` | asks the item to show its own menu |
| `scroll(id, delta, horizontal)` | wheel |
| `menu(id)` | the DBusMenu tree: `label`, `icon`, `enabled`, `separator`, checkmark/radio `toggle`, `children` |
| `activate_menu(id, entry)` | runs a menu entry |

Draw `menu(id)` yourself to get menus that match the rest of your shell.
