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
