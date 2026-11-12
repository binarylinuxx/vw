---
title: Styling
description: Colors, gradients, corners, borders, opacity, text and images — every visual property vw draws, in C++ and Facet.
---

Everything vw draws is a rectangle, text or an image, styled by the props you pass. There are no stylesheets: a theme is a set of colors and sizes your code reuses.

## Colors

```cpp
vw::rgb(0x151a1e)        // opaque
vw::rgba(0x151a1ef0)     // with alpha: the last byte
vw::Color{0.1F, 0.2F, 0.3F, 1.0F}   // RGBA floats, 0–1
```

In Facet: `"#151a1e"`, `"#151a1ef0"`, `"transparent"`, or a `vw::Color` named in C++ (`color: accent`). Colors with alpha blend over whatever is behind them, including other surfaces and the wallpaper.

## Gradients

A rectangle can blend from `color` to `color2` along one axis: `horizontal` runs left to right, `vertical` top to bottom. The blend is linear across the whole rectangle and follows its rounded corners.

```cpp
ui.rectangle({.width = 320, .height = 120, .radius = 24,
              .color = vw::rgb(0xa7cbe7), .color2 = vw::rgb(0xdbddff),
              .gradient = vw::Gradient::horizontal});
```

```facet
ui.Rectangle(width: 320, height: 120, radius: 24,
             color: "#a7cbe7", color2: "#dbddff", gradient: horizontal)
```

Either stop may be transparent, which fades a panel out towards one edge: `.color2 = vw::rgba(0xa7cbe700)`. Gradients take two stops along an axis; angles, more stops or radial fills are a job for a [custom shader](/guides/shaders/).

## Corners

`radius` rounds every corner; `corner_radii` sets them one by one, `{top_left, top_right, bottom_right, bottom_left}`, and unset ones keep `radius`:

```cpp
ui.rectangle({.width = 280, .height = 56, .radius = 16,
              .corner_radii = {std::nullopt, std::nullopt, 4.0F, 4.0F}});  // a tab: round top, square-ish bottom
```

A radius of half the height makes a pill. Edges are antialiased.

## Borders

`border_width` and `border_color` draw a border inside the rectangle's edge, following its corners. The fill sits within it, so a border never changes the size of the widget.

```cpp
ui.rectangle({.width = 200, .height = 44, .radius = 12, .color = vw::rgba(0x00000000),
              .border_width = 1.5F, .border_color = vw::rgb(0x42494f)});   // an outline only
```

```facet
ui.Rectangle(width: 200, height: 44, radius: 12, color: "transparent",
             border_width: 1.5, border_color: "#42494f")
```

Facet's rows, columns and grids take `bg`, `radius`, `border_width` and `border_color` too, drawn behind their children.

## Opacity and clipping

`ui.fade(widget, opacity)` multiplies the opacity of everything a widget covers — fills, gradients, borders, text and images — so a whole card fades together. `ui.clip(widget, clip)` cuts a widget down to a rounded rectangle. Both are covered with examples in [Clipping, fading, masks](/guides/composition/).

## Text

| Prop | Look |
| --- | --- |
| `font` | a fontconfig family: `"sans"`, `"monospace"`, `"Inter"` |
| `size` | pixels |
| `weight` | `vw::weight::thin`, `light`, `regular`, `medium`, `semibold`, `bold`, `black`, or a number |
| `color` | any color, with alpha for quieter text |
| `max_width`, `max_lines`, `ellipsize` | wrap and cut long text: `none`, `start`, `middle`, `end` |
| `align` | `start`, `center`, `end` for the lines of wrapped text |

Text is shaped with Pango and HarfBuzz, so ligatures, emoji and right-to-left scripts render as the font intends.

## Images

| Prop | Look |
| --- | --- |
| `fit` | `stretch`, `contain` (letterboxed) or `cover` (cropped) |
| `radius` | rounded corners |
| `tint` | multiplies every pixel; white leaves it alone, so `tint` recolours white icons |
| `effect.saturation` | 0 greys it out, 1 is unchanged, above 1 boosts it |
| `effect.hue_rotate` | degrees around the colour wheel |
| `effect.colorize`, `effect.colorize_amount` | recolours towards a colour by luminance, keeping the shading |
| `mask` | clips the image to a polygon |

```cpp
ui.image({.icon = "firefox", .width = 48, .height = 48,
          .effect = {.saturation = 0.0F}});   // a greyed-out, inactive app
```

## Hover

`input.hover` changes a widget's look while the pointer is over it: a `color` to blend to and a `scale`, eased over `duration` seconds.

```cpp
.input = {.on_click = open, .hover = vw::Hover{.color = vw::rgb(0x262d33), .scale = 1.04F}}
```

```facet
input: { on_click: () => actions.open(app.id), hover: { color: "#262d33", scale: 1.04 } }
```

## Beyond the built-ins

vw has no built-in shadows, blur or glows: draw them with a [custom shader](/guides/shaders/), which can read what is already drawn behind it (blxshell's frosted panels and soft shadows are shaders). A [canvas](/reference/widgets/#canvas) draws anything Canvas 2D can, such as charts and arcs.

## Keeping a look consistent

Put your palette and sizes in one place and use them everywhere:

```cpp
namespace theme {
inline const vw::Color surface = vw::rgb(0x151a1e);
inline const vw::Color raised = vw::rgb(0x20272c);
inline const vw::Color accent = vw::rgb(0xa7cbe7);
inline constexpr float radius = 16;
}
```

Facet reads the same constants once they have a plain name, for example through a file-level block:

```facet
include "theme.hpp"
cpp { inline const auto accent = theme::accent; }
…
ui.Rectangle(color: accent, radius: 16)
```

so C++ and Facet views share one theme.
