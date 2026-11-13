---
title: Loops, conditions, let
description: Repeating elements, choosing between them, naming values and elements, and properties blocks.
---

## for

```facet
for app in state.apps key app.id {
    ui.Text(text: app.name)
}
```

`for` repeats its children for each item of any C++ range. `key` names what identifies an item: Facet keys [animations](/facet/animation/) and controls by it, so they follow the item when the list changes order. Without `key`, the item's position is used.

## if

```facet
if state.loading {
    ui.Spinner()
} else if state.error != null {
    ui.Text(text: state.error, color: "#fa746f")
} else {
    ui.Text(text: "Ready")
}
```

A branch may draw nothing; layouts around it cope with an empty list.

## let

```facet
for metric in state.metrics {
    let critical = metric.value >= 90
    ui.Text(text: metric.name, color: critical ? "#fa746f" : "#dfe6ee")
}
```

`let` names a value for the elements after it, in the same block.

At the top of the file, outside any view, `let` makes a constant every view in the file can use:

```facet
let gap = 10
let accent = "#a7cbe7"

view Bar(state) {
    ui.Text(text: state.title, color: accent)
}
```

## Naming elements: id

`id:` with a bare name gives an element a name. Its children and the elements after it in the same block read its box through that name, so sizes and positions follow one another instead of being repeated numbers:

```facet
let gap = 8

ui.Surface(id: bar, width: 0, height: 32, anchor: top) {
    layout.Row(id: row, width: bar.width - gap * 2, height: bar.height, gap: gap, align: center) {
        ui.Text(id: title, text: state.title)
        // whatever the title leaves of the row
        ui.Rectangle(width: row.width - title.width - gap, height: 4, color: "#a7cbe7")
    }
}
```

| Through the name | Is |
| --- | --- |
| `x`, `y`, `width`, `height` | plain numbers, for any element |
| `props` | everything the element was given, plus its defaults: `panel.props.variant` |
| `built` | whether the box is measured yet |

The box depends on where you read it:

- **Inside the element** (its children), it is what the element asked for: `bar.width` above is the monitor's width, because `width: 0` fills it. A size left unset is `0` there.
- **After the element** (later siblings, and anything below it), it is the **measured** box: `title.width` is the width the text actually came out at, and `row.width` the row as laid out.

A name is seen only below where it is given; there is no referring forward. Positions of children inside a `Row`, `Column` or `Grid` are final only once that layout is built, so their siblings see where they were made, not where the layout puts them. Two elements in one block cannot share a name.

On a control, `id: "search"` (a string) or a variable still sets the control's state key, as before; only a name not already in use names the element.

## properties

At the top of the file, `properties(id: name) { … }` sets properties for the element named `id: name`, so a view holds structure and the sizes, colors and places sit apart, like a stylesheet for one element:

```facet
properties(id: panel) {
    variant: "panel"
    width: 200
    height: 100
    place: { x: 10 }
}

view Bar(state, actions) {
    S.Surface(id: panel, place: { y: 20 }) { … }
}
```

Properties are written one per line, with or without commas. What the element sets itself wins; an object set in both places merges field by field, so the panel above sits at `x: 10, y: 20`. Values may use file-level `let`s and other elements' names (`panel.height - 34`), as they could on the element. A `properties` block for a name no element uses is an error.
