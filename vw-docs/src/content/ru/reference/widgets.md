---
title: Виджеты рисования
description: Каждый вызов vw::Ui добавляет примитивы в сцену и возвращает Widget — их диапазон и измеренный размер — для раскладки, обрезки и затухания.
---

Все свойства — агрегаты: задавайте нужное через designated initializers, в порядке объявления.

```cpp
const vw::Widget w = ui.rectangle({.width = 200, .height = 40, .radius = 12,
                                   .color = vw::rgb(0x263b4d), .place = {.x = 8, .y = 8}});
```

Цвета — `std::array<float, 4>` от 0 до 1; их строят `vw::rgb(0xRRGGBB)` и `vw::rgba(0xRRGGBBAA)`.

## Размещение и ввод

Каждый виджет принимает `place`, а большинство — ещё и `input`:

| `Placement` | Значение |
| --- | --- |
| `x`, `y` | положение в пикселях поверхности (если задано, привязывает поверхность к левому верхнему углу) |
| `clip` | `{x, y, width, height, radius}` — по чему обрезать |
| `visible` | `false` оставляет виджет в раскладке, но ничего не рисует |
| `anchor`, `layer`, `margin`, `exclusive`, `exclusive_zone` | устаревшее размещение поверхности через виджет; лучше `ui.surface` |

`input` описан на странице [Ввод](/ru/reference/input/).

## rectangle

| Свойство | Значение |
| --- | --- |
| `width`, `height`, `radius` | размер и радиус углов |
| `corner_radii` | `{top_left, top_right, bottom_right, bottom_left}`; незаданные берут `radius` |
| `color`, `color2`, `gradient` | заливка, вторая точка, `none` / `horizontal` / `vertical` |
| `border_width`, `border_color` | рамка внутрь |
| `shader`, `shader_params`, `shader_data` | [свой шейдер](/ru/guides/shaders/) |

Градиент идёт от `color` к `color2` слева направо (`horizontal`) или сверху вниз (`vertical`) и обрезается по скруглённым углам:

```cpp
ui.rectangle({.width = 320, .height = 120, .radius = 24,
              .color = vw::rgb(0xa7cbe7), .color2 = vw::rgb(0xdbddff),
              .gradient = vw::Gradient::horizontal});
```

Две точки вдоль оси — это всё, что умеют прямоугольники; для углов, большего числа точек или радиальных заливок рисуйте [своим шейдером](/ru/guides/shaders/) или на [canvas](#canvas).

## text

| Свойство | Значение |
| --- | --- |
| `text`, `font`, `size`, `weight` | содержимое и семейство fontconfig, размер в пикселях, насыщенность (`vw::weight::regular`, `semibold`, `bold`, `black`…) |
| `color` | цвет текста |
| `max_width`, `max_lines` | перенос в пределах ширины; `0` строк — без ограничения |
| `align` | `start`, `center`, `end` |
| `ellipsize` | `none`, `start`, `middle`, `end` |

Возвращённый виджет имеет логический размер текста — готов к центрированию через `ui.move`.

## image

| Свойство | Значение |
| --- | --- |
| `path` | файл (PNG, JPEG, WebP, SVG, …) |
| `icon` | имя иконки темы или сериализованный GIcon, ищется через тему иконок |
| `bitmap` | пиксели в памяти, например [захват экрана](/ru/compositor/capture/) |
| `width`, `height`, `fit` | `0` сохраняет естественный размер или пропорции; `fit` — `stretch`, `contain` или `cover` |
| `radius`, `mask`, `tint`, `effect` | см. [Обрезка, затухание, маски](/ru/guides/composition/) |

## canvas

Скрипт на JavaScript (QuickJS) рисует через API в духе Canvas 2D на поверхность cairo. `code` — исходник, либо `script` — файл, читаемый и кэшируемый по времени изменения. `id` — ключ среды выполнения; `animate = true` вызывает `draw(ctx, canvas, time)` в каждом кадре.

## text_field

Однострочный редактор; см. [Окна приложений](/ru/guides/application/#текстовые-поля). `authenticate = true` делает его полем пароля для экрана блокировки.

## Утилиты

| Вызов | Значение |
| --- | --- |
| `ui.hit_area({width, height, place, input})` | невидимая область ввода |
| `ui.move(widget, dx, dy)` | сдвинуть построенный виджет |
| `ui.hide(widget)` | скрыть построенный виджет |
| `ui.clip(widget, clip)`, `ui.fade(widget, opacity)` | см. [Обрезка, затухание, маски](/ru/guides/composition/) |
