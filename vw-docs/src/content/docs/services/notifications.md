---
title: Notifications
description: vw is an org.freedesktop.Notifications daemon. It owns the model — open, popping up, kept — so a shell only draws it.
---

```cpp
vw::notifications::start();   // claims the bus name unless another daemon has it
app.on_event(vw::notifications::event_fd(), vw::notifications::drain_events);

for (const auto& n : vw::notifications::popups())  draw_popup(n);
for (const auto& n : vw::notifications::history()) draw_in_centre(n);
```

## Lifecycle

| Happens | State afterwards |
| --- | --- |
| `Notify` arrives | open; pops up (with do-not-disturb on, only critical ones do) |
| `hide_popup(id)`, `hide_popups()`, enabling do-not-disturb | open, no longer popping up, still kept |
| its timeout runs out | closed (reason 1), still kept |
| `dismiss(id)`, a non-resident action, `clear()` | closed (reason 2), gone |
| the sender calls `CloseNotification` | closed (reason 3), gone |

Transient notifications are never kept. The history holds at most 256.

## Reading

| Function | Returns, newest first |
| --- | --- |
| `list()` | open notifications |
| `popups()` | what should be popping up now |
| `history()` | what a notification centre keeps |
| `generation()` | a counter bumped on every change |

Each list is a cached snapshot rebuilt only after a change, and theme icons are resolved once per notification, so calling these in every build is cheap. Call them from the drawing thread.

## Acting

| Function | Effect |
| --- | --- |
| `dismiss(id)` | the user dismissed it |
| `hide_popup(id)`, `hide_popups()` | stop popping up |
| `invoke(id, key)` | sends `ActionInvoked`; then dismisses unless resident |
| `activate(id)` | the default action: `default` if offered, else the only action |
| `clear()` | dismisses everything |
| `set_do_not_disturb(bool)`, `do_not_disturb()` | enabling hides current popups |

## Notification

`id`, `app_name`, `app_icon`, `summary`, `body` (may carry the spec's markup subset), `actions` (`{key, label}`), `urgency` (0–2), `value` (progress hint, −1 without), `category`, `desktop_entry`, `resident`, `transient`, `action_icons`, `popup`, `closed`, `close_reason`, `timestamp` (Unix seconds), `expire_timeout` (ms; 0 never).

`icon` and `image` are ready-to-draw paths: the app's icon (from `app_icon` or the desktop entry) and the notification's own picture (`image-data`, written to a PNG that lives with the notification, or `image-path`).

## Spec behaviour

- `replaces_id` replaces in place and is always the returned id.
- A synchronous tag (`x-canonical-private-synchronous`, `x-dunst-stack-tag`, `synchronous`) replaces an open notification with the same tag from the same app — volume and brightness indicators rely on it.
- Default timeout is 5 s; critical notifications persist; an explicit 0 never expires.
- Capabilities: `actions`, `body`, `body-markup`, `icon-static`, `persistence`.
