---
title: Network, Bluetooth, power
description: NetworkManager, BlueZ, UPower with power profiles, and backlights — each a cached snapshot with an event fd.
---

## Network — `vw/network.hpp`

| Function | Meaning |
| --- | --- |
| `status()` | `Status`: `wifi_enabled`, `wired`, `connectivity`, `primary` kind, `connection`, `address`, `ssid`, `strength` |
| `access_points()` | `ssid`, `bssid`, `strength`, `frequency`, `secured`, `active`, `saved` |
| `icon_name(status)` | a matching symbolic icon name |
| `set_wifi_enabled(bool)`, `scan()` | radio and scanning |
| `connect(ssid, password)`, `disconnect_wifi()` | uses a saved connection when there is one |

## Bluetooth — `vw/bluetooth.hpp`

| Function | Meaning |
| --- | --- |
| `adapter()`, `adapters()` | `powered`, `discovering`, `discoverable`, name, address |
| `devices()` | `name`, `icon`, `paired`, `trusted`, `connected`, `busy`, `battery` |
| `set_powered`, `set_discovering` | adapter control |
| `connect`, `disconnect`, `pair`, `forget` | by device path |
| `last_error()` | why the last call failed |

## Power — `vw/upower.hpp`

| Function | Meaning |
| --- | --- |
| `display_device()` | the composite battery UPower shows: `percentage`, `state`, `time_to_empty`, `time_to_full`, `icon_name` |
| `devices()` | every device, including mice, headsets and keyboards |
| `on_battery()` | running on battery |
| `profiles()`, `profile()`, `set_profile(name)` | power-profiles-daemon |

## Backlights — `vw/brightness.hpp`

| Function | Meaning |
| --- | --- |
| `screen()`, `keyboard()`, `all()` | `Backlight {subsystem, name, brightness, max_brightness, fraction()}` |
| `set_screen(fraction)`, `change_screen(delta)`, `set(backlight, fraction)` | writes go through logind, no root needed |

Brightness watches the sysfs files, so changes made elsewhere arrive as events too.
