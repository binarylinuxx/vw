---
title: Compositors and protocols
description: What vw asks of a Wayland compositor, what it uses when a compositor offers more, and which parts are zwwm's own.
---

vw is a Wayland client. Each surface opens its own connection, binds the globals it needs, and checks for optional ones. A compositor that lacks a required protocol gets a clear error naming it; a missing optional one just turns that feature off.

## Required

| Protocol | For |
| --- | --- |
| `wl_compositor`, `zwp_linux_dmabuf_v1` | every surface: vw renders with Vulkan and hands the compositor dma-bufs |
| `wl_seat` | pointer, keyboard (with xkbcommon keymaps and key repeat) |
| `zwlr_layer_shell_v1` or zwwm's `zwwm_layer_shell_v1` | layer surfaces: bars, docks, overlays; zwwm's is preferred when present |
| `xdg_wm_base` | [application windows](/guides/application/) |
| `ext_session_lock_manager_v1`, `wl_output` | [lock screens](/guides/locking/): one lock surface per output |

Only the role a surface uses must exist: a bar needs layer shell, not xdg-shell.

## Used when offered

| Protocol | For |
| --- | --- |
| `zwlr_data_control_manager_v1` | the clipboard without keyboard focus; else `wl_data_device_manager` |
| `zwlr_screencopy_manager_v1` | [output capture](/compositor/capture/) |

## zwwm's own

| Protocol | For |
| --- | --- |
| `ext_zwwm_manager_v1`, `zwwm_tags_v1` | [tags, snapshots, actions](/compositor/zwwm/) |
| `zwwm_edge_swipe_manager_v1` | [touch swipes from screen edges](/compositor/edge-swipes/) |
| `zwwm_focus_grab_manager_v1` | [popups that close on an outside click](/compositor/zwwm-surfaces/#focus-grabs) |
| `zwwm_decoration_manager_v1` | [server-side window decorations](/compositor/zwwm-surfaces/#decorations) |
| `zwwm_capture_access_v1`, then `zwwm_screencopy_view_manager_v1` and `ext_zwwm_toplevels_v1` | [per-window capture](/compositor/capture/#windows), for clients holding the session's capture token |

On Hyprland, vw reads compositor state over its IPC sockets instead; see [Hyprland](/compositor/hyprland/).

## Surface roles inside vw

`WaylandWidget` (`src/wayland.cpp`) owns the connection, input, clipboard and frame pacing. How content becomes a surface is a role in `src/roles/`: `layer.cpp`, `toplevel.cpp`, `lock.cpp`, each behind the `Role` interface in `role.hpp`. A new kind of surface (a popup, a subsurface) is one more file there and one case in `make_role`.
