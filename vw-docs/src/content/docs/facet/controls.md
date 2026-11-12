---
title: Controls
description: Toggle, Slider, Dropdown and Spinner — ready-made controls in vw.ui, built from vw primitives.
---

The controls keep their small bits of state (a slider's drag, a dropdown's open list) keyed by their place in the file and the loops around them, so each one in a list is separate. `id` overrides the key.

| Control | Properties |
| --- | --- |
| `Toggle` | `value`, `on_change(bool)`, `width`, `height`, `color` (on), `off_color`, `thumb_color` |
| `Slider` | `value`, `min`, `max`, `step` (0: continuous), `on_change(number)`, `width`, `height`, `color`, `track_color`, `thumb_color` |
| `Dropdown` | `options` (a list), `current` (or `value`), `on_change(text)`, `width`, `height`, `color`, `hover_color`, `text_color` |
| `Spinner` | `size`, `color` |

```facet
for setting in state.settings key setting.id {
    layout.Row(justify: space-between, align: center, width: 360) {
        ui.Text(text: setting.label)
        if setting.kind == "toggle" {
            ui.Toggle(value: setting.on, on_change: (on) => actions.set(setting.id, on))
        } else {
            ui.Slider(value: setting.level, on_change: (level) => actions.level(setting.id, level))
        }
    }
}
```

A control reports changes; your actions store them, and the next build shows the new value.
