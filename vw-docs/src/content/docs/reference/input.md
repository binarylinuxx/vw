---
title: Input
description: Pointer input is per widget; keys go to one handler per surface and then to the focused text field.
---

## vw::Input

| Field | Called with |
| --- | --- |
| `on_click` | `ClickEvent {x, y, button}` — 272 left, 273 right, 274 middle |
| `hover` | `Hover {color, scale, duration}`: a tint and scale animated without rebuilding |
| `on_hover` | `bool inside` — entering and leaving, even under other widgets; rebuilds afterwards |
| `on_drag_start`, `on_drag`, `on_drag_end` | `DragEvent {x, y, start_x, start_y, dx, dy, delta_x, delta_y, button, cancelled}` |
| `on_scroll` | `ScrollEvent {x, y, delta}` — positive scrolls down |
| `drag_button` | which button drags (BTN_LEFT, 272) |

The topmost widget under the pointer receives clicks. A drag captures the pointer until release. Handlers usually change your state; the scene rebuilds after every handler runs.

## Keyboard

```cpp
ui.keyboard([&](const vw::KeyEvent& key) {
  if (key.name == "Escape") { open = false; return true; }
  if (key.name == "Down") { ++selected; return true; }
  return false;  // the focused text field gets it
}, vw::Keyboard::exclusive);
```

`KeyEvent` has `keysym`, `name` (xkb names: `Escape`, `Return`, `Down`, `a`, `F5`), `text` (UTF-8, if any), modifier flags `ctrl`, `alt`, `shift`, `super`, and `repeat` for key-repeat events. The mode applies to layer surfaces; see [Shells and layer surfaces](/guides/shells/#keyboard).
