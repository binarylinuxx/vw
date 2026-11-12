---
title: Application windows
description: The same build function becomes an xdg-toplevel window with a title, app id, resizing, text fields and the clipboard.
---

Call `ui.toplevel(props)` instead of `ui.surface(props)`:

```cpp
vw::App app([&](vw::Ui& ui) {
  const vw::Size size = vw::toplevel_size();  // the compositor's configure wins
  ui.rectangle({.width = (float)size.width, .height = (float)size.height,
                .color = vw::rgb(0x101419)});
  ui.text_field({.id = "query", .placeholder = "Search…", .width = 320, .autofocus = true,
                 .on_submit = [](const std::string& text) { run(text); },
                 .place = {.x = 24, .y = 24}});
  ui.toplevel({.width = 640, .height = 420, .title = "Finder", .app_id = "my.finder",
               .on_close = [] { /* save, then let the app exit */ }});
});
```

- `width` and `height` are the initial size; after the compositor configures the window, `vw::toplevel_size()` is the truth. Build against it so resizing just works.
- Without `on_close`, closing the window ends the app.
- `vw::request_toplevel_size(w, h)` asks for a new size; `vw::request_toplevel_close()` closes from code.
- A surface's role is fixed: one surface cannot switch between toplevel, layer and lock on reload.

## Text fields

`text_field` is a complete single-line editor: caret, selection by drag, double-click selects a word and triple-click everything, Home/End, word jumps and word deletion with Ctrl, Ctrl+A/C/X/V through the system clipboard, and password masking. Its value lives under `id`; `value` only seeds it the first time. `vw::reset_text_field(id)` forgets it (for example when a dialog closes).

| Prop | Meaning |
| --- | --- |
| `on_change`, `on_submit` | called with the new text / on Enter |
| `password` | shows bullets, never copies |
| `autofocus` | focused as soon as it appears |
| `max_length` | in bytes, default 4096 |
| `focus_color`, `border_color`, `caret_color`, `selection_color` | styling |

## Clipboard

`vw::set_clipboard(text)` and `vw::clipboard_text()` use the surface that last received input (Wayland needs an input serial), via `wlr-data-control` when available.
