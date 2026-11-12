---
title: Surfaces and roles
description: A build picks its surface's role — layer surface, window or lock — by calling one of four functions. The role cannot change afterwards.
---

| Call | Role | Props |
| --- | --- | --- |
| `ui.surface(props)` | layer surface | `SurfaceProps` |
| `ui.toplevel(props)` | window | `ToplevelProps` |
| `ui.session_lock(props)` + `ui.lock_surface(props)` | session lock, one surface per output | `SessionLockProps`, `LockSurfaceProps` |

A build that draws nothing leaves the surface unmapped.

## SurfaceProps

| Field | Default | Meaning |
| --- | --- | --- |
| `width`, `height` | content size | `0` fills that axis; read the result with `vw::surface_size()` |
| `anchor` | `center` | edges, as `vw::anchor` bits |
| `layer` | `top` | `background`, `bottom`, `top`, `overlay` |
| `margin` | 0 | `{top, right, bottom, left}` |
| `exclusive_zone` | 0 | pixels reserved from the anchored edge; `-1` ignores others' zones |
| `surface_namespace` | `"vw"` | for compositor layer rules |
| `input_regions` | everything | `{x, y, w, h}` rectangles that take input; empty is click-through |
| `focus_grab`, `on_activated`, `on_dismiss` | off | zwwm focus grab for popups |
| `on_closed` | — | the compositor closed the surface |
| `refresh_interval` | 1 s | periodic rebuilds, 0.008–3600 s |

## ToplevelProps

| Field | Default | Meaning |
| --- | --- | --- |
| `width`, `height` | content size | initial size |
| `refresh_interval` | 1 s | periodic rebuilds |
| `title`, `app_id` | `"vw"` | window metadata |
| `on_close` | exit | the window was closed |

## SessionLockProps and LockSurfaceProps

| Field | Meaning |
| --- | --- |
| `pam_service` | PAM service; `swaylock` if configured, else `login` |
| `pam_conf` | an absolute path to a PAM file of your own |
| `on_unlock` | the session unlocked; without it the app exits |
| `width`, `height` (lock surface) | defaults to the output size |
