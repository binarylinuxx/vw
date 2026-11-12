---
title: Layout
description: Layout happens after building — widgets are measured first, then rows, columns, stacks and anchor layouts move them into place. Lists render only what is visible.
---

## row, column, stack

```cpp
const vw::Widget icon = ui.image({.icon = "audio-volume-high", .width = 20, .height = 20});
const vw::Widget label = ui.text({.text = "68%", .size = 14});
ui.row({icon, label}, {.gap = 8, .padding = 10, .radius = 18, .place = {.x = 12, .y = 6}});
```

`row` and `column` take children and `LayoutProps` (`gap`, `padding`, optional fixed `width`/`height`, `radius`, `place`, `input`). Children are centred on the cross axis unless their `place.anchor` asks for an edge (`top`/`bottom` in a row, `left`/`right` in a column); their `margin` is respected. `stack` overlays children in one box. Each returns one widget covering everything, which you can lay out again.

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
