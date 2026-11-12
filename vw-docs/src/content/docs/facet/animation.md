---
title: Animation
description: Tweening a value with animate(), keyed by its place in the file and the loops around it.
---

```facet
ui.Rectangle(width: animate(360 * state.progress / 100, 200), height: 6, color: "#a7cbe7")
```

`animate(target, milliseconds[, easing])` returns a value moving towards `target`; vw keeps frames coming until it arrives. It works anywhere a number does. Easings: `linear`, `in_out_cubic`, `out_cubic` (default), `out_quint`, `out_expo`, `out_back`, `spring`.

Each `animate` is keyed by its line and column plus the `key` of every `for` around it, so each item of a list moves on its own:

```facet
for task in state.tasks key task.id {
    ui.Rectangle(width: animate(task.done ? 0 : 280, 160, out_back), height: 36)
}
```

The property form animates one property of an element:

```facet
ui.Rectangle(width: 368 * state.progress / 100, animate: { property: "width", duration: 200 })
```
