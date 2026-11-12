---
title: App and the loop
description: vw::App owns the surfaces, the event sources and the loop. vw/vw.hpp includes the whole public API.
---

```cpp
#include <vw/vw.hpp>

int main() {
  vw::App app;
  app.surface(build_bar)
     .surface(build_launcher)
     .on_event(vw::hyprland::event_fd(), vw::hyprland::drain_events);
  return app.run();
}
```

## vw::App

| Member | Meaning |
| --- | --- |
| `App()` / `App(build)` | an empty app, or one with a single surface |
| `surface(build)` | adds a surface built by `void(vw::Ui&)`; chainable |
| `on_event(fd, drain)` | rebuilds every surface when `fd` is readable and `drain()` returns true; negative fds are ignored |
| `run()` | runs until closed; returns `EXIT_SUCCESS`, or prints the error and returns `EXIT_FAILURE` |
| `build()` | builds the first surface once — check a scene without Wayland |

## When a surface rebuilds

| Trigger | Which surfaces |
| --- | --- |
| input on a surface | that one at once, the rest before the loop sleeps |
| an event source fired | all of them |
| its `refresh_interval` elapsed (default 1 s) | only that one |
| a frame callback while animating | only that one |
| SIGHUP | all of them |

SIGINT and SIGTERM end `run()` cleanly.

## Lower level

`vw::SceneSource` is the interface the loop drives: `build()` returns a `vw::Scene`, `event_sources()` lists fds, and `watch_directory()`/`affects(name)` let a source rebuild on file edits (the Lua frontend uses this for hot reload). `vw::run(source)` runs one. Most programs never need it.

## Host state

Functions in `vw/host.hpp` describe the surface being built:

| Function | Returns |
| --- | --- |
| `vw::surface_size()` | the configured size of the layer surface being built, `{0, 0}` before the first configure |
| `vw::toplevel_size()` | the window's configured size |
| `vw::lock_surface_context()` | width, height and output name of the lock surface being built |
| `vw::session_locked()`, `vw::session_authentication_failed()` | lock state |
| `vw::focus_grab_state()` | whether zwwm's focus grab is available and active |
