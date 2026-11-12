---
title: Layout
description: Layout happens after building — widgets are measured first, then rows, columns, stacks and anchor layouts move them into place. Lists render only what is visible.
---

## Positioning and centering

Every widget starts at the top-left of the surface. To put it somewhere else:

| Want | Use |
| --- | --- |
| centred, or against an edge of the surface | `ui.align(widget, vw::anchor::center)`, with `left`, `right`, `top`, `bottom` bits and a `{top, right, bottom, left}` margin |
| a fixed spot | `.place = {.x = 24, .y = 8}` in its props |
| nudged after it is built | `ui.move(widget, dx, dy)` |
| arranged with others | a `row`, `column`, `grid` or `stack` below |

A bar with the date on the left, a clock in the middle and the network on the right:

```cpp
const vw::Widget date = ui.text({.text = "Thu 12"});
const vw::Widget clock = ui.text({.text = "23:01"});
const vw::Widget net = ui.text({.text = "Wired connection 1"});
ui.align(date, vw::anchor::left, {0, 0, 0, 12});
ui.align(clock);                                    // centre, both ways
ui.align(net, vw::anchor::right, {0, 12, 0, 0});
ui.surface({.width = 0, .height = 32, .anchor = vw::anchor::top});
```

`align` measures against the configured surface when there is one (here the full-width bar), else against everything drawn so far. The clock stays in the middle whatever the widths of its neighbours, which `justify = space_between` only does when they match.

## row, column, stack

```cpp
const vw::Widget icon = ui.image({.icon = "audio-volume-high", .width = 20, .height = 20});
const vw::Widget label = ui.text({.text = "68%", .size = 14});
ui.row({icon, label}, {.gap = 8, .padding = 10, .radius = 18, .place = {.x = 12, .y = 6}});
```

`row` and `column` take children and `LayoutProps` (`gap`, `padding`, optional fixed `width`/`height`, `justify`, `align`, `radius`, `place`, `input`). `stack` overlays children in one box. Each returns one widget covering everything, which you can lay out again.

When a fixed `width` (row) or `height` (column) leaves room, `justify` places the children along the axis:

| `justify` | Children |
| --- | --- |
| `start` (default), `center`, `end` | packed at that end, `gap` apart |
| `space_between` | first and last at the edges, the rest spread between |
| `space_around` | equal space around each, half of it at the edges |
| `space_evenly` | equal space between them and at the edges |

`align` (`start`, `center` by default, `end`) places them across the axis. A child's own `place.anchor` wins: `top`/`bottom` in a row, `left`/`right` in a column. Margins are respected.

```cpp
// A title bar: title on the left, buttons on the right.
ui.row({title, ui.row({minimise, close}, {.gap = 6})},
       {.width = 480, .padding = 12, .justify = vw::Justify::space_between});
```

## grid

`grid(children, {.columns, .gap, .row_gap, .padding, .width, .height, .align})` fills `columns` cells per row, left to right. Columns are as wide as the widest child, or share a fixed `width` equally; each row is as tall as its tallest child. `align` places a child inside its cell on both axes; `row_gap` defaults to `gap`.

```cpp
std::vector<vw::Widget> tiles;
for (const auto& app : apps) tiles.push_back(app_tile(ui, app));
ui.grid(tiles, {.columns = 4, .gap = 16, .width = 560});
```

## anchor_layout

For QML-style anchoring inside a box:

```cpp
ui.anchor_layout({.width = 400, .height = 240, .children = {
  {.left = true, .top = true, .margin = {12, 0, 0, 12},
   .render = [&](const vw::AnchorBox& box) { return ui.text({.text = "Title"}); }},
  {.left = true, .right = true, .bottom = true, .height = 48, .margin = {0, 12, 12, 12},
   .render = [&](const vw::AnchorBox& box) {
     return ui.rectangle({.width = box.width, .height = box.height, .radius = 12}); }},
}});
```

Anchoring both edges of an axis stretches the child; `min_*`/`max_*` bound it. `render` receives the resolved box and builds the child, which is then moved into place.

## scrollbox

`scrollbox(children, {.id, .width, .height, .gap, .padding, .scroll_step})` stacks children vertically in a clipped viewport that scrolls with the wheel or touchpad. The offset is kept under `id`; `vw::scroll_offset(id)` and `vw::set_scroll_offset(id, y)` read and set it.

## listview

Virtualised: `render(index)` runs only for rows inside the viewport, so a list of thousands costs a screenful.

```cpp
ui.listview({.id = "apps", .width = 360, .height = 480, .item_height = 52, .gap = 4,
             .count = apps.size(),
             .render = [&](std::size_t i) -> std::optional<vw::Widget> { return row_for(apps[i]); },
             .keep_visible = selected});
```

- `row_height(i)` gives rows different heights (called for every row; keep it cheap).
- `keep_visible` scrolls just enough to bring that row fully into view when it changes — keyboard selection.
- Returning `std::nullopt` skips a row.
