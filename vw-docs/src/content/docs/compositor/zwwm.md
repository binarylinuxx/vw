---
title: Tags, snapshots, actions
description: zwwm's manager and tags protocols from vw/zwwm.hpp, on a connection of their own, with an event fd for the loop.
---

`vw/zwwm.hpp` talks to zwwm over its own Wayland connection. Calls throw when the protocol they need is missing; `available()` says whether zwwm is there at all.

| Function | Meaning |
| --- | --- |
| `available()` | zwwm is running |
| `tags()`, `active_tags()` | tags with `id`, `name`, `active` |
| `set_active_tags(ids)` | shows tags on the focused output |
| `snapshot(type)` | JSON for `status`, `outputs`, `toplevels`, `output_tags`, `layers`, `cameras`, `keyboard`, `live` |
| `dispatch(action, argument)` | runs a compositor action; returns `{success, message}` |
| `reload()`, `set_cursor(theme, size)`, `rebuild_switch_shaders()` | compositor control |
| `last_event()` | the most recent subscription event |
| `event_fd()`, `drain_events()` | for `app.on_event`: readable when compositor state changes |

## Rebuilding on events

zwwm streams events many times a second; compare what you draw (tags, the focused window) before deciding to rebuild. A snapshot is a round trip to the compositor: take one per change, not one per frame.

```cpp
app.on_event(vw::zwwm::event_fd(), [] {
  if (!vw::zwwm::drain_events()) return false;
  static std::vector<std::uint32_t> last;
  auto now = vw::zwwm::active_tags();
  if (now == last) return false;
  last = std::move(now);
  return true;  // rebuild
});
```

## Window state bits

`snapshot(Snapshot::toplevels)` reports each window's `state` as bits: `1` mapped, `2` focused, `4` floating, `8` fullscreen, `16` modal dialog, `32` hidden. A shell that hides itself for fullscreen windows checks the focused window's bit 8.
