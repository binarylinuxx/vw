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
| `on_scroll_x` | `ScrollEvent` sideways: tilt wheels, touchpads, horizontal swipes; positive scrolls right |
| `on_touch` | `TouchEvent {phase, id, x, y, start_x, start_y, fingers}` — raw fingers, see [Touch](#touch) |
| `drag_button` | which button drags (BTN_LEFT, 272) |

The topmost widget under the pointer receives clicks. A drag captures the pointer until release. Handlers usually change your state; the scene rebuilds after every handler runs.

## Touch

Touchscreens drive the same handlers, one finger at a time, with nothing extra to write:

| Finger | Becomes |
| --- | --- |
| a tap | `on_click` (button 272), on the widget under it |
| a drag starting on something with `on_drag…` | that drag, as with the mouse |
| a drag starting in a text field | moving the caret and selecting |
| a swipe anywhere else, past 10 px | scrolling what is under it: up and down to `on_scroll`, `scrollbox` and `listview`, sideways to `on_scroll_x`; the press no longer clicks |

A widget shows its hover look while touched, and nothing stays hovered after the finger lifts. If the compositor takes the touch for a gesture of its own, the press is dropped and a drag ends with `cancelled`. One finger at a time drives the widgets this way; further fingers go to `on_touch`, or nowhere.

### Several fingers: on_touch

A widget with `on_touch` gets its fingers raw, every one that goes down on it, each with its own `id`, from `down` through `motion` to `up` (or `cancel`). They do not also click, drag or scroll it. `fingers` counts the fingers held by `on_touch` widgets, so a pinch is two of them:

```cpp
std::map<std::int32_t, std::pair<double, double>> held;
double zoom = 1, from_spread = 0, from_zoom = 1;
const auto spread = [&] {
  auto a = held.begin()->second, b = std::next(held.begin())->second;
  return std::hypot(a.first - b.first, a.second - b.second);
};
// in the build
ui.rectangle({.width = 200 * zoom, .height = 200, .input = {.on_touch = [&](const vw::TouchEvent& touch) {
  using Phase = vw::TouchEvent::Phase;
  if (touch.phase == Phase::up || touch.phase == Phase::cancel) { held.erase(touch.id); return; }
  held[touch.id] = {touch.x, touch.y};
  if (touch.phase == Phase::down && held.size() == 2) { from_spread = spread(); from_zoom = zoom; }
  if (held.size() == 2 && from_spread > 0) zoom = from_zoom * spread() / from_spread;
}}});
```

Motion rebuilds with the next frame, as a drag does; the other phases rebuild at once.

## On-screen keyboard

Text fields speak `text-input-v3`, so an on-screen keyboard (squeekboard, maliit, any input method) works with them as it does with any app:

- Focusing a text field asks for the keyboard; leaving it, or no field having focus, puts it away. Tapping a focused field brings back a keyboard that was dismissed.
- What the keyboard is still composing shows in the field as you type and is replaced when it commits; it reaches `on_change` like typed text.
- Passwords and lock-screen fields are marked sensitive: the keyboard learns nothing of their text, and gets no word suggestions for them.
- The keyboard's own Enter submits the field: `on_submit`, or unlocking on a lock screen.
- The keyboard is told where the caret is drawn, so it can keep clear of it.

## Keyboard

```cpp
ui.keyboard([&](const vw::KeyEvent& key) {
  if (key.name == "Escape") { open = false; return true; }
  if (key.name == "Down") { ++selected; return true; }
  return false;  // the focused text field gets it
}, vw::Keyboard::exclusive);
```

`KeyEvent` has `keysym`, `name` (xkb names: `Escape`, `Return`, `Down`, `a`, `F5`), `text` (UTF-8, if any), modifier flags `ctrl`, `alt`, `shift`, `super`, and `repeat` for key-repeat events. The mode applies to layer surfaces; see [Shells and layer surfaces](/guides/shells/#keyboard).
