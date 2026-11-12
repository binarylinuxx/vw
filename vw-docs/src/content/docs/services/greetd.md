---
title: Login (greetd)
description: A greetd client for writing a login screen — prompts, answers and starting the session, plus the users and sessions to offer.
---

[greetd](https://sr.ht/~kennylevinsen/greetd/) authenticates and starts sessions; a greeter only asks the questions. `vw/greetd.hpp` is that greeter's side: greetd runs your program with `$GREETD_SOCK` set, and the replies arrive on an event fd, so the screen stays responsive while PAM thinks.

| Function | Meaning |
| --- | --- |
| `available()` | greetd is running this program |
| `begin(username)` | starts a login, cancelling one in progress |
| `state()` | `stage`, `prompt`, `message`, `username`, `denied` |
| `answer(text)` | answers the current prompt; `info` and `error` prompts take none |
| `start(command, env)` | starts the authenticated session, with extra `KEY=value` environment |
| `cancel()` | abandons the login, back to `idle` |
| `event_fd()`, `drain_events()` | for `app.on_event`: greetd replied |
| `users()` | accounts that can log in: login-range uids with a real shell |
| `sessions()` | installed Wayland, then X sessions: `name`, `command`, `file`, `wayland` |

## The flow

`stage` walks `idle` → `waiting` → `prompting` (greetd asks something) → … → `authenticated` → `started`. Each answer goes back to `waiting` until greetd replies. A login may ask several questions: a password, then a one-time code.

| `prompt` | Show |
| --- | --- |
| `secret` | a hidden field: a password |
| `visible` | a plain field: a username, a code |
| `info`, `error` | the message, and a way to go on (`answer()` with no text) |

`failed` ends a login: `message` says why, and `denied` tells wrong credentials from an error in greetd. `begin()` again to retry; vw cancels the failed session for you.

```cpp
app.on_event(vw::greetd::event_fd(), vw::greetd::drain_events);

// in the build function
const auto login = vw::greetd::state();
if (login.stage == vw::greetd::Stage::authenticated)
  vw::greetd::start(chosen_session.command);
if (login.stage == vw::greetd::Stage::started) std::exit(0);  // greetd takes over
```

The password field is an ordinary `ui.text_field` with `password = true`; on submit, `vw::greetd::answer(text)`.

## Running it

greetd starts the greeter inside a compositor of its own, for example in `/etc/greetd/config.toml`:

```toml
[default_session]
command = "cage -s -- my-greeter"
user = "greeter"
```

To develop without logging out, greetd's `fakegreet` runs a greeter against a pretend daemon: `fakegreet ./my-greeter`. It asks for a password (`password`) and then `7 + 2` (`9`), for user `user`.
