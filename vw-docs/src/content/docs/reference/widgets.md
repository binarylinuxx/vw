---
title: Drawing widgets
description: Every call on vw::Ui appends primitives to the scene and returns a Widget — its range and measured size — for layout, clipping and fading.
---

All props are aggregates: set what you need with designated initializers, in declaration order.

```cpp
const vw::Widget w = ui.rectangle({.width = 200, .height = 40, .radius = 12,
                                   .color = vw::rgb(0x263b4d), .place = {.x = 8, .y = 8}});
```

Colours are `std::array<float, 4>` in 0–1; `vw::rgb(0xRRGGBB)` and `vw::rgba(0xRRGGBBAA)` build them.

## Placement and input

Every widget takes `place` and most take `input`:

| `Placement` | Meaning |
| --- | --- |
| `x`, `y` | position in surface pixels (anchors the surface top-left when set) |
| `clip` | `{x, y, width, height, radius}` to clip to |
| `visible` | `false` keeps it in the layout but draws nothing |
| `anchor`, `layer`, `margin`, `exclusive`, `exclusive_zone` | legacy per-widget surface placement; prefer `ui.surface` |

`input` is described on the [Input](/reference/input/) page.

## rectangle

| Prop | Meaning |
| --- | --- |
| `width`, `height`, `radius` | size and corner radius |
| `corner_radii` | `{top_left, top_right, bottom_right, bottom_left}`; unset ones use `radius` |
| `color`, `color2`, `gradient` | fill, second stop, `none` / `horizontal` / `vertical` |
| `border_width`, `border_color` | an inside border |
| `shader`, `shader_params`, `shader_data` | a [custom shader](/guides/shaders/) |

## text

| Prop | Meaning |
| --- | --- |
| `text`, `font`, `size`, `weight` | content and fontconfig family, pixel size, weight (`vw::weight::regular`, `semibold`, `bold`, `black`…) |
| `color` | text colour |
| `max_width`, `max_lines` | wrap within a width; `0` lines is unlimited |
| `align` | `start`, `center`, `end` |
| `ellipsize` | `none`, `start`, `middle`, `end` |

The returned widget has the text's logical size, ready for centring with `ui.move`.

## image

| Prop | Meaning |
| --- | --- |
| `path` | a file (PNG, JPEG, WebP, SVG, …) |
| `icon` | a theme icon name or serialized GIcon, resolved through the icon theme |
| `bitmap` | pixels in memory, such as a [screen capture](/compositor/capture/) |
| `width`, `height`, `fit` | `0` keeps the natural size or aspect; `fit` is `stretch`, `contain` or `cover` |
| `radius`, `mask`, `tint`, `effect` | see [Clipping, fading, masks](/guides/composition/) |

## canvas

A JavaScript (QuickJS) script draws with a Canvas 2D-style API onto a cairo surface. `code` is the source, or `script` a file read and cached by modification time. `id` keys the runtime; `animate = true` calls `draw(ctx, canvas, time)` every frame.

## text_field

A single-line editor; see [Application windows](/guides/application/#text-fields). `authenticate = true` makes it a lock-screen password field.

## Utilities

| Call | Meaning |
| --- | --- |
| `ui.hit_area({width, height, place, input})` | an invisible input region |
| `ui.move(widget, dx, dy)` | shift a built widget |
| `ui.hide(widget)` | hide a built widget |
| `ui.clip(widget, clip)`, `ui.fade(widget, opacity)` | see [Clipping, fading, masks](/guides/composition/) |
