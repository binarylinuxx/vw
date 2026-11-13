---
title: Медиа и звук
description: Медиаплееры MPRIS с сессионной шины и узлы PipeWire для громкости, выключения звука и устройств по умолчанию.
---

## MPRIS — `vw/mpris.hpp`

Состояние плееров обновляется по их сигналам в фоновом потоке; позиция между сигналами экстраполируется.

| Функция | Значение |
| --- | --- |
| `active()` | плеер для показа: играющий (лучше с обложкой), иначе последний активный, иначе первый |
| `players()` | все плееры |
| `available()` | есть ли активный плеер |
| `previous()`, `play_pause()`, `next()` | управление активным плеером; false, если он не может |
| `set_position(seconds)` | перемотка активного плеера |
| `event_fd()`, `drain_events()` | пересборка при изменениях |

У `Player` есть `service`, `name`, `title`, `artist`, `album`, `art_url`, `lyrics` (если плеер их отдаёт), `track_id`, `playback_status`, `position`, `length` (секунды) и `can_go_previous`, `can_play`, `can_pause`, `can_go_next`.

## PipeWire — `vw/pipewire.hpp`

| Функция | Значение |
| --- | --- |
| `available()`, `error()` | состояние подключения |
| `nodes(filter)` | `all`, `sinks`, `sources` или `streams` |
| `default_sink()`, `default_source()` | устройства по умолчанию |
| `set_volume(id, volume)` | воспринимаемая громкость, от 0 до 4 (1 — это 100%; кубическая шкала, как у ползунков на десктопе) |
| `set_mute(id, muted)`, `toggle_mute(id)` | выключение звука |
| `set_default_sink(id)`, `set_default_source(id)` | смена устройств по умолчанию |

У `Node` есть `id`, `name`, `description`, `media_class`, `app_name`, `state`, `channel_volumes`, `muted`, `can_control`, плюс `volume()` (воспринимаемая) и `linear_volume()`. Запись возвращает `Result {ok, error}`.

Сервис целиком работает на событиях PipeWire; ничего не опрашивается. Чтение возвращает последний снимок и никогда не ждёт PipeWire, поэтому сразу после запуска списки на мгновение пусты и заполняются по событию. Когда PipeWire перезапускается, устройства пропадают и возвращаются, как только снова появляется его сокет, — держите `event_fd()` зарегистрированным: переподключение происходит в `drain_events()`.
