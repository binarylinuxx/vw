---
title: Media and audio
description: MPRIS media players from the session bus, and PipeWire nodes for volume, mute and default devices.
---

## MPRIS — `vw/mpris.hpp`

Player state is refreshed from player signals on a background thread; position is extrapolated between them.

| Function | Meaning |
| --- | --- |
| `active()` | the player to show: a playing one (preferring one with artwork), else the last active, else the first |
| `players()` | every player |
| `available()` | whether there is an active player |
| `previous()`, `play_pause()`, `next()` | controls on the active player; false when it cannot |
| `set_position(seconds)` | seeks the active player |
| `event_fd()`, `drain_events()` | rebuild on changes |

`Player` has `service`, `name`, `title`, `artist`, `album`, `art_url`, `lyrics` (when the player exposes them), `track_id`, `playback_status`, `position`, `length` (seconds) and `can_go_previous`, `can_play`, `can_pause`, `can_go_next`.

## PipeWire — `vw/pipewire.hpp`

| Function | Meaning |
| --- | --- |
| `available()`, `error()` | connection state |
| `nodes(filter)` | `all`, `sinks`, `sources` or `streams` |
| `default_sink()`, `default_source()` | the defaults |
| `set_volume(id, volume)` | perceptual volume, 0 to 4 (1 is 100%; cubic, like desktop sliders) |
| `set_mute(id, muted)`, `toggle_mute(id)` | mute |
| `set_default_sink(id)`, `set_default_source(id)` | change defaults |

`Node` has `id`, `name`, `description`, `media_class`, `app_name`, `state`, `channel_volumes`, `muted`, `can_control`, plus `volume()` (perceptual) and `linear_volume()`. Writes return a `Result {ok, error}`.

The service runs entirely on PipeWire's events; nothing polls. Reads return the latest snapshot and never wait on PipeWire, so right after start the lists are empty for a moment and fill in with an event. When PipeWire restarts, devices disappear, then come back as soon as its socket reappears — keep `event_fd()` registered, since reconnecting happens in `drain_events()`.
