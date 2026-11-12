---
title: Lock screens
description: ext-session-lock-v1 with one surface per output, PAM built into the password field, and a lifecycle that cannot leak the desktop.
---

## Lock

A build function that calls `ui.session_lock()` asks the compositor to lock the session; `ui.lock_surface()` marks what to draw on each output.

```cpp
bool locked = false;  // set by a keybind, a signal, an idle timer…

app.surface([&](vw::Ui& ui) {
  if (!locked) return;                       // draws nothing: no lock
  ui.session_lock({.on_unlock = [&] { locked = false; }});
  ui.lock_surface();
  const vw::LockSurfaceContext output = vw::lock_surface_context();
  const float w = output.width, h = output.height;
  ui.rectangle({.width = w, .height = h, .color = vw::rgb(0x101419)});  // opaque first
  ui.text_field({.id = "password", .placeholder = "Password", .width = 320,
                 .password = true, .authenticate = true, .autofocus = true,
                 .place = {.x = (w - 320) / 2, .y = h * 0.6f}});
});
```

The function runs once per output; `lock_surface_context()` says which one and how big it is. Draw something fully opaque first — nothing behind a lock may ever show.

## Authenticate

A text field with `authenticate = true` checks its value with PAM when Enter is pressed. Success unlocks the session, destroys the lock surfaces and calls `on_unlock`; clear whatever made your build draw the lock there. Without `on_unlock`, unlocking ends the app.

- `vw::session_authentication_failed()` is true after a wrong password, until the next attempt — shake the field, show an error.
- `pam_service` defaults to `swaylock` when `/etc/pam.d/swaylock` exists, otherwise `login`; `pam_conf` can point at a PAM file of your own.
- `on_authenticated(bool)` on the field reports every attempt.

## Habits that help

- Make the first key only reveal the password field instead of typing into it — a habitual space or Enter should not land in the password.
- Clear the field after a period without input.
- Keep entrance animations on top of the opaque base, never instead of it.
