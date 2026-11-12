---
title: Processes and IPC
description: Detached processes, commands polled off the UI thread, Unix socket requests, and PAM.
---

## Processes — `vw/process.hpp`

| Function | Meaning |
| --- | --- |
| `spawn(argv)` | starts a detached process in its own session (stdin from /dev/null); returns the pid |
| `spawn_shell(command)` | the same through `sh -c` |
| `poll_command(command, seconds)` | the last output of a command re-run on a worker thread every `seconds`; returns immediately |
| `poll_event_fd()`, `drain_poll_events()` | fires when a polled command's output changes (`vw::App` registers it for you) |

Never run a blocking command inside a build function: it stalls every surface. Use `poll_command`, or your own thread that writes to an eventfd you register with `on_event`.

## Unix sockets — `vw/ipc.hpp`

`unix_request(path, payload, timeout_ms)` connects, sends, and reads the full response, with send and receive timeouts. `UniqueFd` is a small owning file descriptor.

## PAM — `vw/auth.hpp`

`authenticate_password(password, service, pam_conf)` checks a password directly. Lock screens rarely need it: a text field with `authenticate = true` does this for you.
