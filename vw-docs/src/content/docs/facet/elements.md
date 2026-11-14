---
title: Elements
description: Every vw.ui and vw.layout element and its properties.
---

Colors take `"#rrggbb"`, `"#rrggbbaa"`, `"transparent"` or a C++ `vw::Color`; sizes take numbers or [`"N%"`](/facet/layout/#relative-sizes). `input` is `{ on_click, on_hover, on_scroll, on_scroll_x, on_touch, on_drag_start, on_drag, on_drag_end, hover: { color, scale } }`.

## vw.ui

| Element | Properties |
| --- | --- |
| `Surface` | `width`, `height`, `anchor` (`center`, `top`, `bottom`, `left`, `right`, `top_left`, … `fill`), `layer` (`background`, `bottom`, `top`, `overlay`), `exclusive_zone`, `namespace`, `refresh`, `output` (a monitor's name) |
| `Rectangle` | `width`, `height`, `radius`, `color`/`bg`, `color2` and `gradient` (`horizontal`, `vertical`), `border_width`, `border_color`, `input`; children sit centred on it |
| `Text` | `text` (or first), `font`, `size`, `weight` (`thin` … `black`), `color`, `max_width`, `max_lines`, `align`, `padding`, `input` |
| `Image` | `path` (or first), `icon`, `width`, `height`, `fit` (`stretch`, `contain`, `cover`), `radius`, `padding`, `input`; an empty path keeps its size |
| `TextField` | `id`, `value`, `placeholder`, `width`, `height`, `radius`, `color`, `border_color`, `focus_color`, `text_color`, `placeholder_color`, `font`, `size`, `password`, `autofocus`, `on_change`, `on_submit` |
| `Toggle`, `Slider`, `Dropdown`, `Spinner` | see [Controls](/facet/controls/) |

`Text`'s `align` is `start`, `center` or `end` for its lines, or a place in its parent: `left`, `right`, `top`, `bottom`, `center_left`, `center_right`, `top_left`, … `bottom_right`.

## vw.layout

| Element | Properties |
| --- | --- |
| `Row`, `Column` | `gap`, `padding`, `width`, `height`, `justify`, `align`, `radius`, `bg`/`color`, `border_width`, `border_color` |
| `Grid` | `columns`, `gap`, `row_gap`, `padding`, `width`, `height`, `align`, `radius`, `bg`/`color`, `border_width`, `border_color` |
| `ListView` | `count`, `width`, `height`, `item_height`, `gap`, `radius`, `scroll_step`, `id`; one `(index) => { … }` row |

Any element inside a row or column also takes [`flex`](/facet/layout/#flex).
