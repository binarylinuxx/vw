---
title: Layout and sizing
description: Rows, columns, grids and lists; justify and align; backgrounds; relative sizes and flex.
---

## Rows, columns, grids

```facet
layout.Row(gap: 8, padding: 12, width: 480, justify: space-between, align: center) { … }
layout.Grid(columns: 4, gap: 16, width: 560) { … }
```

`justify` places children along the axis when the row or column is longer than they are: `start`, `center`, `end`, `space-between`, `space-around`, `space-evenly`. `align` places them across it: `start`, `center`, `end`. A grid fills `columns` cells per row; its columns share a fixed `width` equally.

## Backgrounds

```facet
layout.Column(gap: 8, padding: 16, bg: "#151a1e", radius: 16) { … }
```

`bg` (or `color`), `radius`, `border_width` and `border_color` on a row, column or grid draw a rounded background sized to its content. A `ui.Rectangle` with children puts them, centred, on top of it.

## Relative sizes

```facet
layout.Column(width: 600, padding: 20) {
    ui.TextField(width: "100%")   // 560: the column's width less its padding
}
```

`"N%"` is a share of the content box around the element: the nearest parent's width or height, less its padding. A parent without that size passes on its own parent's. If no ancestor has one, Facet says so at the line.

## flex

```facet
layout.Row(gap: 16) {
    ui.Rectangle(flex: 1, height: 80) { … }
    ui.Rectangle(flex: 2, height: 80) { … }
}
```

`flex` shares what the other children leave of a row's width (or a column's height) in proportion. A row or column with flex children spans its parent. Flex children are written directly inside it, not in a `for` or `if`.

## Lists

```facet
layout.ListView(count: state.files.length, height: 300) {
    (index) => {
        let file = state.files[index]
        ui.Text(text: file.name)
    }
}
```

A `ListView` draws only the rows in view and scrolls the rest. Its width defaults to the space around it; without `item_height` the first row is measured; without `height` it shows every row instead of scrolling.
