---
title: Hyprland
description: Hyprland's IPC from vw/hyprland.hpp — queries, dispatches and the event socket.
---

On Hyprland, vw reads compositor state over Hyprland's own sockets rather than Wayland protocols.

| Function | Meaning |
| --- | --- |
| `available()` | `HYPRLAND_INSTANCE_SIGNATURE` points at a running instance |
| `query(endpoint)` | JSON from `j/<endpoint>`: `activeworkspace`, `clients`, `monitors`, `devices`… |
| `request(command)` | a raw socket request |
| `dispatch(args)`, `keyword(args)` | `{ok, response}` |
| `last_event()` | the latest socket2 event `{name, data, sequence}` |
| `event_fd()`, `drain_events()` | for `app.on_event` |

The event thread reconnects by itself when Hyprland restarts. Queries are blocking: cache results and invalidate them from the event handler.

Layer surfaces, lock screens and output capture work on Hyprland through the standard protocols on [Compositors and protocols](/compositor/overview/).
