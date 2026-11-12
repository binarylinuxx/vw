---
title: Screen and window capture
description: Output capture through wlr-screencopy, and per-window capture on zwwm for trusted clients — drawn with ui.image.
---

## Outputs — `vw/capture.hpp`

`vw::capture_output({.output, .region, .cursor})` returns a `vw::Bitmap` of what an output shows, through wlr-screencopy (zwwm and other wlroots-style compositors). Draw it with `ui.image({.bitmap = shot, .width = 320, .height = 180, .fit = vw::ImageFit::cover})`; vw downscales a bitmap once and reuses it while the pixels stay the same.

| Field | Meaning |
| --- | --- |
| `output` | a connector name such as `HDMI-A-1`; empty takes the first output |
| `region` | `{x, y, width, height}` in the output's logical coordinates; unset captures it all |
| `cursor` | include the pointer |

A full 1080p capture takes about one frame, so take one on a refresh interval or when an event says the screen changed, not in every build.

## Windows

zwwm captures single windows for clients that prove they belong to the session: at startup it writes a random token to `$XDG_RUNTIME_DIR/<WAYLAND_DISPLAY>.capture-token`, readable only by you, and names it in `ZWWM_CAPTURE_TOKEN`. vw presents it through `zwwm_capture_access_v1` on its own; there is nothing to configure.

| Function | Meaning |
| --- | --- |
| `window_capture_available()` | zwwm granted window capture |
| `capture_windows()` | `id`, `title`, `app_id`, `x`, `y`, `width`, `height`, `output`, `activated`, `minimized`, `maximized`, `fullscreen` |
| `capture_window(id, cursor)` | the window at its own size, without decorations, also on hidden tags |
| `capture_event_fd()`, `drain_capture_events()` | the window list or a window changed; for `app.on_event` |

An overview captures each window when it opens and again on a slow interval; the event fd says when titles, sizes or the list changed.
