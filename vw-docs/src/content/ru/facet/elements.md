---
title: Элементы
description: Все элементы vw.ui и vw.layout и их свойства.
---

Цвета принимают `"#rrggbb"`, `"#rrggbbaa"`, `"transparent"` или `vw::Color` из C++; размеры — числа или [`"N%"`](/ru/facet/layout/#относительные-размеры). `input` — это `{ on_click, on_hover, on_scroll, on_drag_start, on_drag, on_drag_end, hover: { color, scale } }`.

## vw.ui

| Элемент | Свойства |
| --- | --- |
| `Surface` | `width`, `height`, `anchor` (`center`, `top`, `bottom`, `left`, `right`, `top_left`, … `fill`), `layer` (`background`, `bottom`, `top`, `overlay`), `exclusive_zone`, `namespace`, `refresh`, `output` (имя монитора) |
| `Rectangle` | `width`, `height`, `radius`, `color`/`bg`, `color2` и `gradient` (`horizontal`, `vertical`), `border_width`, `border_color`, `input`; дети лежат на нём по центру |
| `Text` | `text` (или первым), `font`, `size`, `weight` (`thin` … `black`), `color`, `max_width`, `max_lines`, `align`, `padding`, `input` |
| `Image` | `path` (или первым), `icon`, `width`, `height`, `fit` (`stretch`, `contain`, `cover`), `radius`, `padding`, `input`; пустой путь сохраняет размер |
| `TextField` | `id`, `value`, `placeholder`, `width`, `height`, `radius`, `color`, `border_color`, `focus_color`, `text_color`, `placeholder_color`, `font`, `size`, `password`, `autofocus`, `on_change`, `on_submit` |
| `Toggle`, `Slider`, `Dropdown`, `Spinner` | см. [Элементы управления](/ru/facet/controls/) |

`align` у `Text` — это `start`, `center` или `end` для его строк, либо место в родителе: `left`, `right`, `top`, `bottom`, `center_left`, `center_right`, `top_left`, … `bottom_right`.

## vw.layout

| Элемент | Свойства |
| --- | --- |
| `Row`, `Column` | `gap`, `padding`, `width`, `height`, `justify`, `align`, `radius`, `bg`/`color`, `border_width`, `border_color` |
| `Grid` | `columns`, `gap`, `row_gap`, `padding`, `width`, `height`, `align`, `radius`, `bg`/`color`, `border_width`, `border_color` |
| `ListView` | `count`, `width`, `height`, `item_height`, `gap`, `radius`, `scroll_step`, `id`; одна строка `(index) => { … }` |

Любой элемент внутри строки или колонки также принимает [`flex`](/ru/facet/layout/#flex).
