---
title: Сеть, Bluetooth, питание
description: NetworkManager, BlueZ, UPower с профилями питания и подсветка — у каждого кэшированный снимок и fd событий.
---

## Сеть — `vw/network.hpp`

| Функция | Значение |
| --- | --- |
| `status()` | `Status`: `wifi_enabled`, `wired`, `connectivity`, тип `primary`, `connection`, `address`, `ssid`, `strength` |
| `access_points()` | `ssid`, `bssid`, `strength`, `frequency`, `secured`, `active`, `saved` |
| `icon_name(status)` | подходящее имя символьной иконки |
| `set_wifi_enabled(bool)`, `scan()` | радио и сканирование |
| `connect(ssid, password)`, `disconnect_wifi()` | использует сохранённое соединение, если оно есть |

## Bluetooth — `vw/bluetooth.hpp`

| Функция | Значение |
| --- | --- |
| `adapter()`, `adapters()` | `powered`, `discovering`, `discoverable`, имя, адрес |
| `devices()` | `name`, `icon`, `paired`, `trusted`, `connected`, `busy`, `battery` |
| `set_powered`, `set_discovering` | управление адаптером |
| `connect`, `disconnect`, `pair`, `forget` | по пути устройства |
| `last_error()` | почему последний вызов не удался |

## Питание — `vw/upower.hpp`

| Функция | Значение |
| --- | --- |
| `display_device()` | составная батарея, которую показывает UPower: `percentage`, `state`, `time_to_empty`, `time_to_full`, `icon_name` |
| `devices()` | все устройства, включая мыши, гарнитуры и клавиатуры |
| `on_battery()` | работа от батареи |
| `profiles()`, `profile()`, `set_profile(name)` | power-profiles-daemon |

## Подсветка — `vw/brightness.hpp`

| Функция | Значение |
| --- | --- |
| `screen()`, `keyboard()`, `all()` | `Backlight {subsystem, name, brightness, max_brightness, fraction()}` |
| `set_screen(fraction)`, `change_screen(delta)`, `set(backlight, fraction)` | запись идёт через logind, root не нужен |

Яркость следит за файлами sysfs, так что изменения, сделанные в другом месте, тоже приходят событиями.
