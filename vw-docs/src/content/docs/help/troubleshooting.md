---
title: Troubleshooting
description: The usual reasons a surface does not show, does not take input, or redraws more than it should.
---

## Nothing appears

- **No layer-shell.** vw needs `wlr-layer-shell-v1` (or zwwm's) for `ui.surface`. GNOME does not offer it; Sway, Hyprland, river, niri and zwwm do.
- **The build drew nothing.** An empty scene leaves the surface unmapped by design. Check the condition that gates your popup.
- **A fill-size surface before configure.** `vw::surface_size()` is `{0, 0}` until the compositor answers; lay out against a fallback until then.
- **A custom shader fails to load.** The error names the file and the problem: a missing file, a corrupt `.vws` (checksum mismatch), or a newer format version. Recompile it with `vw-shaderc`.

## Clicks fall through, or never do

- `input_regions` is unset → the whole surface takes input; an empty list → none of it does.
- A widget without `input` is not hit-testable; the topmost widget with input under the pointer wins.
- Layer surfaces in `background`/`bottom` sit under windows: clicks only reach them where no window is.

## Keys do not arrive

Layer surfaces only receive keys if they ask: call `ui.keyboard(handler, vw::Keyboard::on_demand)` (or `exclusive` for a launcher), or include a text field.

## It redraws too much

- Each surface rebuilds on its `refresh_interval` (1 s by default). Raise it for static surfaces.
- A tween whose target changes every build never settles. Feed it a stable target.
- Blocking calls in a build function stall every surface: compositor queries, `popen`, file reads. Cache them, invalidate from an event source, or move them to a thread with an eventfd.
- A settled shell should sit at 0 % CPU between events; if it does not, something keeps animating or polling.

## Notifications do not show

`vw::notifications::start()` returns an error when another daemon (mako, dunst, swaync) already owns `org.freedesktop.Notifications`. Stop it first.
