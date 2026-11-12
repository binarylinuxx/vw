---
title: Ввод
description: Ввод указателя — у каждого виджета свой; клавиши идут одному обработчику на поверхность, а потом в текстовое поле с фокусом.
---

## vw::Input

| Поле | Вызывается с |
| --- | --- |
| `on_click` | `ClickEvent {x, y, button}` — 272 левая, 273 правая, 274 средняя |
| `hover` | `Hover {color, scale, duration}`: оттенок и масштаб, анимируемые без пересборки |
| `on_hover` | `bool inside` — вход и выход, даже под другими виджетами; после этого пересборка |
| `on_drag_start`, `on_drag`, `on_drag_end` | `DragEvent {x, y, start_x, start_y, dx, dy, delta_x, delta_y, button, cancelled}` |
| `on_scroll` | `ScrollEvent {x, y, delta}` — положительное значение прокручивает вниз |
| `drag_button` | какая кнопка тянет (BTN_LEFT, 272) |

Клики получает самый верхний виджет под указателем. Перетаскивание захватывает указатель до отпускания. Обработчики обычно меняют ваше состояние; сцена пересобирается после каждого обработчика.

## Клавиатура

```cpp
ui.keyboard([&](const vw::KeyEvent& key) {
  if (key.name == "Escape") { open = false; return true; }
  if (key.name == "Down") { ++selected; return true; }
  return false;  // достанется текстовому полю с фокусом
}, vw::Keyboard::exclusive);
```

У `KeyEvent` есть `keysym`, `name` (имена xkb: `Escape`, `Return`, `Down`, `a`, `F5`), `text` (UTF-8, если есть), флаги модификаторов `ctrl`, `alt`, `shift`, `super` и `repeat` для событий автоповтора. Режим действует для layer-поверхностей; см. [Оболочки и layer-поверхности](/ru/guides/shells/#клавиатура).
