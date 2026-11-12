---
title: Карта модулей
description: Каждый модуль Lua-фронтенда, доступный через require(), и чему он соответствует в C++.
---

| Модуль | Функции | C++ |
| --- | --- | --- |
| `vw.base` | `rectangle`, `text`, `image`, `canvas`, `text_field`, `hit_area` (`drag`), `row`, `column`, `stack`, `scrollbox`, `listview`, `anchor_layout`, `repeater`, `lazy` | `vw::Ui` |
| `vw.surface` | `set`, `focus_state` | `Ui::surface`, `focus_grab_state` |
| `vw.toplevel` | `set`, `size`, `close` | `Ui::toplevel`, `toplevel_size` |
| `vw.session` | `lock`, `locksurface`, `surface`, `locked`, `failed`, `authenticate` | блокировка сеанса |
| `vw.animation` | `value`, `request_frame` | `vw::animate` |
| `vw.datetime` | `format`, `monotonic`, `unix` | — |
| `vw.poll` | `command` | `vw::poll_command` |
| `vw.process` | `spawn` | `vw::spawn` |
| `vw.socket` | `available`, `request`, `json` | `vw::unix_request` |
| `vw.json`, `vw.path` | помощники | — |
| `vw.services.applications` | `list`, `search`, `launch` | `vw::applications` |
| `vw.services.mpris` | `state`, `players`, `available`, `previous`, `play_pause`, `next`, `set_position` | `vw::mpris` |
| `vw.services.pipewire` | `available`, `error`, `nodes`, `default_sink`, `default_source`, сеттеры | `vw::pipewire` |
| `vw.services.notifications` | `start`, `running`, `list`, `popups`, `history`, `generation`, `dismiss`, `hide_popup`, `hide_popups`, `invoke`, `activate`, `clear`, `dnd`, `set_dnd` | `vw::notifications` |
| `vw.services.hyprland` | `available`, `query`, `request`, `last_event` | `vw::hyprland` |
| `vw.services.zwwm` | `available`, `tags`, `active_tags`, `set_active_tags`, `last_event` | `vw::zwwm` |

`repeater` строит по виджету на каждый элемент списка; `lazy` строит своего ребёнка через `render()` и прячет его, пока `visible` ложно. У NetworkManager, BlueZ, UPower, подсветки и трея нет привязок к Lua: для них используйте API на C++.
