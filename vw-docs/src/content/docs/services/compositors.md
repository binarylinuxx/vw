---
title: Compositors
description: Typed state from zwwm's protocols, and Hyprland's IPC — each with an event fd.
---

## zwwm — `vw/zwwm.hpp`

| Function | Meaning |
| --- | --- |
| `available()` | zwwm is running |
| `tags()`, `active_tags()` | tags with `id`, `name`, `active` |
| `set_active_tags(ids)` | shows tags on the focused output |
| `snapshot(type)` | JSON for `status`, `outputs`, `toplevels`, `output_tags`, `layers`, `cameras`, `keyboard`, `live` |
| `dispatch(action, argument)` | runs a compositor action; returns `{success, message}` |
| `reload()`, `set_cursor(theme, size)`, `rebuild_switch_shaders()` | compositor control |
| `watch_edge_swipes(edges)` | follow touch swipes from `Edge::top`, `bottom`, `left`, `right` (ORed); `0` stops |
| `take_edge_swipes()` | swipes since the last call: `phase` (`began`, `progress`, `ended`), `edge`, `distance`, `lateral`, `velocity`, `cancelled` |

Edge swipes arrive through `event_fd()` like every other zwwm event. The compositor keeps the touch away from other clients; drag a panel with `distance` while the swipe is in progress and decide on `ended` from `distance` and `velocity`:

```cpp
vw::zwwm::watch_edge_swipes(vw::zwwm::left);
app.on_event(vw::zwwm::event_fd(), [] {
  const bool changed = vw::zwwm::drain_events();
  for (const auto& swipe : vw::zwwm::take_edge_swipes()) {
    sidebar_offset = swipe.phase == vw::zwwm::EdgeSwipe::Phase::ended ? 0 : swipe.distance;
    if (swipe.phase == vw::zwwm::EdgeSwipe::Phase::ended && !swipe.cancelled)
      sidebar_open = swipe.distance > 120 || swipe.velocity > 600;
  }
  return changed;
});
```

## Screen capture — `vw/capture.hpp`

`vw::capture_output({.output, .region, .cursor})` returns a `vw::Bitmap` of what an output shows, through wlr-screencopy (zwwm and other wlroots-style compositors). Draw it with `ui.image({.bitmap = shot, .width = 320, .height = 180, .fit = vw::ImageFit::cover})`; vw downscales a bitmap once and reuses it while the pixels stay the same.

A full 1080p capture takes about one frame, so take one on a refresh interval or when an event says the screen changed, not in every build.

### Windows

zwwm captures single windows for clients that prove they belong to the session: at startup it writes a random token to `$XDG_RUNTIME_DIR/<WAYLAND_DISPLAY>.capture-token`, readable only by you, and names it in `ZWWM_CAPTURE_TOKEN`. vw presents it on its own; nothing to configure.

| Function | Meaning |
| --- | --- |
| `window_capture_available()` | zwwm granted window capture |
| `capture_windows()` | `id`, `title`, `app_id`, `x`, `y`, `width`, `height`, `output`, `activated`, `minimized`, `maximized`, `fullscreen` |
| `capture_window(id, cursor)` | the window at its own size, without decorations, also on hidden tags |
| `capture_event_fd()`, `drain_capture_events()` | the window list or a window changed; for `app.on_event` |

An overview captures each window when it opens and again on a slow interval; the event fd says when titles, sizes or the list changed.

zwwm streams events many times a second; compare what you draw (tags, the focused window) before deciding to rebuild. A snapshot is a round trip to the compositor — take one per change, not one per frame.

## Hyprland — `vw/hyprland.hpp`

| Function | Meaning |
| --- | --- |
| `available()` | `HYPRLAND_INSTANCE_SIGNATURE` points at a running instance |
| `query(endpoint)` | JSON from `j/<endpoint>`: `activeworkspace`, `clients`, `monitors`, `devices`… |
| `request(command)` | a raw socket request |
| `dispatch(args)`, `keyword(args)` | `{ok, response}` |
| `last_event()` | the latest socket2 event `{name, data, sequence}` |

The event thread reconnects by itself when Hyprland restarts. Queries are blocking: cache results and invalidate them from the event handler.
