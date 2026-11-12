---
title: Clipping, fading, masks
description: Shape what a group of widgets shows — clip it to a rounded box, fade all of it at once, or cut an image to any polygon.
---

## Draw order

Primitives are drawn in the order you create them, text and images included. Draw a panel after some text and the panel covers it. Draw backgrounds first, then their contents, then overlays.

## Clip a widget

Any widget's range can be clipped after it is built:

```cpp
const vw::Widget card = /* rectangles, text, images … */;
ui.clip(card, {.x = x, .y = y, .width = w, .height = shown_height, .radius = 24});
```

Everything in the range, and its input, is clipped to the rounded box; primitives entirely outside are hidden. This is how a card's contents stay inside the card while its height animates. A single primitive can also be clipped at creation with `place.clip`.

To build one widget out of several helpers, bracket them: create a zero-sized `hit_area` before and after and take the range between — or use a layout (`stack`, `column`) that returns one widget.

## Fade a widget

```cpp
ui.fade(card, 1.0f - std::abs(swipe) / width);
```

`fade` multiplies the opacity of everything in the range — fills, gradients, borders, text and images — so a whole card can fade as it is swiped away.

## Shape images

`ImageProps` takes several ways to shape a picture:

| Prop | Effect |
| --- | --- |
| `radius` | rounded corners |
| `fit` | `stretch`, `contain` (letterbox) or `cover` (crop, centred) |
| `mask` | a closed polygon `x0, y0, x1, y1, …` from 0 to 1 across the image, anti-aliased; flatten curves into enough points first |
| `tint` | multiplies the colour |
| `effect.saturation` | 1 unchanged, 0 grey, above 1 vivid |
| `effect.hue_rotate` | degrees around the colour wheel |
| `effect.colorize`, `colorize_amount` | recolours by luminance, keeping shading |

```cpp
ui.image({.path = cover_art, .width = 96, .height = 96, .fit = vw::ImageFit::cover,
          .effect = {.saturation = 0.6f, .colorize = theme.primary, .colorize_amount = 0.3f},
          .mask = scalloped_polygon, .place = {.x = 16, .y = 16}});
```

Images are decoded and fitted once per file, modification time, fit and size, so moving or clipping an image every frame does not re-decode it — but animating its size does.
