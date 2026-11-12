---
title: Оформление
description: Цвета, градиенты, углы, рамки, прозрачность, текст и картинки — каждое визуальное свойство, которое рисует vw, на C++ и в Facet.
---

Всё, что рисует vw, — это прямоугольник, текст или картинка, оформленные переданными свойствами. Таблиц стилей нет: тема — это набор цветов и размеров, которые переиспользует ваш код.

## Цвета

```cpp
vw::rgb(0x151a1e)        // непрозрачный
vw::rgba(0x151a1ef0)     // с альфой: последний байт
vw::Color{0.1F, 0.2F, 0.3F, 1.0F}   // RGBA числами от 0 до 1
```

В Facet: `"#151a1e"`, `"#151a1ef0"`, `"transparent"` или `vw::Color`, названный в C++ (`color: accent`). Цвета с альфой смешиваются с тем, что под ними, включая другие поверхности и обои.

## Градиенты

Прямоугольник может переходить от `color` к `color2` вдоль одной оси: `horizontal` идёт слева направо, `vertical` — сверху вниз. Переход линейный по всему прямоугольнику и повторяет его скруглённые углы.

```cpp
ui.rectangle({.width = 320, .height = 120, .radius = 24,
              .color = vw::rgb(0xa7cbe7), .color2 = vw::rgb(0xdbddff),
              .gradient = vw::Gradient::horizontal});
```

```facet
ui.Rectangle(width: 320, height: 120, radius: 24,
             color: "#a7cbe7", color2: "#dbddff", gradient: horizontal)
```

Любая из точек может быть прозрачной — так панель растворяется к одному краю: `.color2 = vw::rgba(0xa7cbe700)`. Градиенты берут две точки вдоль оси; углы, больше точек или радиальные заливки — работа для [своего шейдера](/ru/guides/shaders/).

## Углы

`radius` скругляет все углы; `corner_radii` задаёт их по одному, `{top_left, top_right, bottom_right, bottom_left}`, а незаданные берут `radius`:

```cpp
ui.rectangle({.width = 280, .height = 56, .radius = 16,
              .corner_radii = {std::nullopt, std::nullopt, 4.0F, 4.0F}});  // вкладка: круглый верх, почти квадратный низ
```

Радиус в половину высоты даёт «таблетку». Края сглажены.

## Рамки

`border_width` и `border_color` рисуют рамку внутри края прямоугольника, повторяя его углы. Заливка лежит внутри неё, так что рамка никогда не меняет размер виджета.

```cpp
ui.rectangle({.width = 200, .height = 44, .radius = 12, .color = vw::rgba(0x00000000),
              .border_width = 1.5F, .border_color = vw::rgb(0x42494f)});   // только контур
```

```facet
ui.Rectangle(width: 200, height: 44, radius: 12, color: "transparent",
             border_width: 1.5, border_color: "#42494f")
```

Строки, колонки и сетки Facet тоже принимают `bg`, `radius`, `border_width` и `border_color` — это рисуется позади их детей.

## Прозрачность и обрезка

`ui.fade(widget, opacity)` умножает непрозрачность всего, что покрывает виджет, — заливок, градиентов, рамок, текста и картинок, — так что вся карточка гаснет целиком. `ui.clip(widget, clip)` обрезает виджет до скруглённого прямоугольника. Оба описаны с примерами в [Обрезка, затухание, маски](/ru/guides/composition/).

## Текст

| Свойство | Вид |
| --- | --- |
| `font` | семейство fontconfig: `"sans"`, `"monospace"`, `"Inter"` |
| `size` | в пикселях |
| `weight` | `vw::weight::thin`, `light`, `regular`, `medium`, `semibold`, `bold`, `black` или число |
| `color` | любой цвет, с альфой для более тихого текста |
| `max_width`, `max_lines`, `ellipsize` | перенос и обрезка длинного текста: `none`, `start`, `middle`, `end` |
| `align` | `start`, `center`, `end` для строк перенесённого текста |

Текст шейпится Pango и HarfBuzz, так что лигатуры, эмодзи и письменность справа налево выглядят так, как задумано шрифтом.

## Картинки

| Свойство | Вид |
| --- | --- |
| `fit` | `stretch`, `contain` (с полями) или `cover` (с обрезкой) |
| `radius` | скруглённые углы |
| `tint` | умножает каждый пиксель; белый ничего не меняет, поэтому `tint` перекрашивает белые иконки |
| `effect.saturation` | 0 делает серым, 1 — без изменений, больше 1 усиливает |
| `effect.hue_rotate` | градусы по цветовому кругу |
| `effect.colorize`, `effect.colorize_amount` | перекрашивает к цвету по яркости, сохраняя тени |
| `mask` | обрезает картинку по многоугольнику |

```cpp
ui.image({.icon = "firefox", .width = 48, .height = 48,
          .effect = {.saturation = 0.0F}});   // серое, неактивное приложение
```

## Наведение

`input.hover` меняет вид виджета, пока над ним указатель: `color`, к которому смешивать, и `scale`, с плавностью за `duration` секунд.

```cpp
.input = {.on_click = open, .hover = vw::Hover{.color = vw::rgb(0x262d33), .scale = 1.04F}}
```

```facet
input: { on_click: () => actions.open(app.id), hover: { color: "#262d33", scale: 1.04 } }
```

## За пределами встроенного

Встроенных теней, размытия и свечения в vw нет: рисуйте их [своим шейдером](/ru/guides/shaders/), который может читать то, что уже нарисовано под ним (матовые панели и мягкие тени blxshell — это шейдеры). [Canvas](/ru/reference/widgets/#canvas) рисует всё, что умеет Canvas 2D, например графики и дуги.

## Единый стиль

Держите палитру и размеры в одном месте и используйте их везде:

```cpp
namespace theme {
inline const vw::Color surface = vw::rgb(0x151a1e);
inline const vw::Color raised = vw::rgb(0x20272c);
inline const vw::Color accent = vw::rgb(0xa7cbe7);
inline constexpr float radius = 16;
}
```

Facet читает те же константы, как только у них есть простое имя, например через блок уровня файла:

```facet
include "theme.hpp"
cpp { inline const auto accent = theme::accent; }
…
ui.Rectangle(color: accent, radius: 16)
```

так что представления на C++ и Facet делят одну тему.
