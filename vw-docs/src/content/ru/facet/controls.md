---
title: Элементы управления
description: Toggle, Slider, Dropdown и Spinner — готовые элементы управления в vw.ui, построенные из примитивов vw.
---

Элементы управления хранят свои маленькие кусочки состояния (перетаскивание ползунка, открытый список) по своему месту в файле и циклам вокруг, так что каждый в списке — отдельный. `id` заменяет этот ключ.

| Элемент | Свойства |
| --- | --- |
| `Toggle` | `value`, `on_change(bool)`, `width`, `height`, `color` (включён), `off_color`, `thumb_color` |
| `Slider` | `value`, `min`, `max`, `step` (0: плавно), `on_change(number)`, `width`, `height`, `color`, `track_color`, `thumb_color` |
| `Dropdown` | `options` (список), `current` (или `value`), `on_change(text)`, `width`, `height`, `color`, `hover_color`, `text_color` |
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

Элемент сообщает об изменениях; ваши actions их сохраняют, а следующая сборка показывает новое значение.
