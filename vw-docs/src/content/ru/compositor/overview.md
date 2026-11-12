---
title: Композиторы и протоколы
description: Что vw требует от композитора Wayland, чем пользуется, когда композитор даёт больше, и какие части принадлежат самому zwwm.
---

vw — клиент Wayland. Каждая поверхность открывает своё соединение, привязывает нужные ей глобальные объекты и проверяет необязательные. Если у композитора нет обязательного протокола, вы получите понятную ошибку с его названием; если нет необязательного — просто отключится соответствующая возможность.

## Обязательные

| Протокол | Для чего |
| --- | --- |
| `wl_compositor`, `zwp_linux_dmabuf_v1` | любая поверхность: vw рисует через Vulkan и отдаёт композитору dma-buf |
| `wl_seat` | указатель, клавиатура (с раскладками xkbcommon и автоповтором) |
| `zwlr_layer_shell_v1` или `zwwm_layer_shell_v1` от zwwm | layer-поверхности: панели, доки, оверлеи; вариант zwwm предпочтительнее, если он есть |
| `xdg_wm_base` | [окна приложений](/ru/guides/application/) |
| `ext_session_lock_manager_v1`, `wl_output` | [экраны блокировки](/ru/guides/locking/): по поверхности блокировки на монитор |

Обязателен только протокол роли, которую использует поверхность: панели нужен layer shell, а не xdg-shell.

## Используются, если есть

| Протокол | Для чего |
| --- | --- |
| `zwlr_data_control_manager_v1` | буфер обмена без фокуса клавиатуры; иначе `wl_data_device_manager` |
| `zwlr_screencopy_manager_v1` | [захват мониторов](/ru/compositor/capture/) |
| `zxdg_output_manager_v1` | логические размеры и положение [мониторов](/ru/reference/app/#мониторы) |

## Собственные протоколы zwwm

| Протокол | Для чего |
| --- | --- |
| `ext_zwwm_manager_v1`, `zwwm_tags_v1` | [теги, снимки, действия](/ru/compositor/zwwm/) |
| `zwwm_edge_swipe_manager_v1` | [свайпы от краёв экрана](/ru/compositor/edge-swipes/) |
| `zwwm_focus_grab_manager_v1` | [попапы, закрывающиеся по клику мимо](/ru/compositor/zwwm-surfaces/#захват-фокуса) |
| `zwwm_decoration_manager_v1` | [декорации окон на стороне сервера](/ru/compositor/zwwm-surfaces/#декорации) |
| `zwwm_capture_access_v1`, затем `zwwm_screencopy_view_manager_v1` и `ext_zwwm_toplevels_v1` | [захват отдельных окон](/ru/compositor/capture/#окна) для клиентов, у которых есть токен захвата сеанса |

На Hyprland vw вместо этого читает состояние композитора через его IPC-сокеты; см. [Hyprland](/ru/compositor/hyprland/).

## Роли поверхностей внутри vw

`WaylandWidget` (`src/wayland.cpp`) владеет соединением, вводом, буфером обмена и темпом кадров. То, как содержимое становится поверхностью, — это роль в `src/roles/`: `layer.cpp`, `toplevel.cpp`, `lock.cpp`, каждая за интерфейсом `Role` в `role.hpp`. Новый вид поверхности (попап, сабсёрфейс) — это ещё один файл там и один случай в `make_role`.
