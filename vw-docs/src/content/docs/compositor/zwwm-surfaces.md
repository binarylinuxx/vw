---
title: Focus grabs and decorations
description: zwwm's extras for surfaces — popups that know when the user clicks elsewhere, and windows the compositor decorates.
---

## Focus grabs

With zwwm, a layer surface can ask to be told when the user clicks outside it: set `focus_grab = true` in `SurfaceProps`. `on_activated` runs when the grab starts; `on_dismiss` receives a `FocusEvent` whose `reason()` is `outside`, `unmapped`, `replaced`, `locked` or `released`: close your popup there.

```cpp
ui.surface({.layer = vw::Layer::overlay, .focus_grab = menu_open,
            .on_dismiss = [](const vw::FocusEvent&) { menu_open = false; }});
```

`vw::focus_grab_state()` says whether grabs are available and whether one is active. Other compositors ignore `focus_grab`.

## Decorations

`ToplevelProps::decorations` asks zwwm to draw a window's titlebar and borders (`server`) or to leave them to the window (`client`); `automatic` takes the compositor's choice. zwwm answers with what it picked, and may change it later: read `vw::toplevel_server_decorated()` and leave out your own titlebar when it is true. The window rebuilds when the answer changes.

```cpp
ui.toplevel({.title = "Notes", .app_id = "notes", .decorations = vw::Decorations::server});
if (!vw::toplevel_server_decorated()) draw_titlebar(ui);
```
