---
title: Module map
description: Every require()-able module of the Lua frontend and what it maps to in C++.
---

| Module | Functions | C++ |
| --- | --- | --- |
| `vw.base` | `rectangle`, `text`, `image`, `canvas`, `text_field`, `hit_area` (`drag`), `row`, `column`, `stack`, `scrollbox`, `listview`, `anchor_layout`, `repeater`, `lazy` | `vw::Ui` |
| `vw.surface` | `set`, `focus_state` | `Ui::surface`, `focus_grab_state` |
| `vw.toplevel` | `set`, `size`, `close` | `Ui::toplevel`, `toplevel_size` |
| `vw.session` | `lock`, `locksurface`, `surface`, `locked`, `failed`, `authenticate` | session lock |
| `vw.animation` | `value`, `request_frame` | `vw::animate` |
| `vw.datetime` | `format`, `monotonic`, `unix` | — |
| `vw.poll` | `command` | `vw::poll_command` |
| `vw.process` | `spawn` | `vw::spawn` |
| `vw.socket` | `available`, `request`, `json` | `vw::unix_request` |
| `vw.json`, `vw.path` | helpers | — |
| `vw.services.applications` | `list`, `search`, `launch` | `vw::applications` |
| `vw.services.mpris` | `state`, `players`, `available`, `previous`, `play_pause`, `next`, `set_position` | `vw::mpris` |
| `vw.services.pipewire` | `available`, `error`, `nodes`, `default_sink`, `default_source`, setters | `vw::pipewire` |
| `vw.services.notifications` | `start`, `running`, `list`, `popups`, `history`, `generation`, `dismiss`, `hide_popup`, `hide_popups`, `invoke`, `activate`, `clear`, `dnd`, `set_dnd` | `vw::notifications` |
| `vw.services.hyprland` | `available`, `query`, `request`, `last_event` | `vw::hyprland` |
| `vw.services.zwwm` | `available`, `tags`, `active_tags`, `set_active_tags`, `last_event` | `vw::zwwm` |

`repeater` builds a widget per item of a list; `lazy` builds its child with `render()` and hides it while `visible` is false. NetworkManager, BlueZ, UPower, backlights and the tray have no Lua bindings: use the C++ API for those.
