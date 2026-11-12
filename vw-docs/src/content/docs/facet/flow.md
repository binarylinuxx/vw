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

`id:` with a bare name gives an element a name. Its children and the elements after it in the same block read its properties through that name, so sizes and positions can follow one another instead of being repeated numbers:

```facet
S.Surface(id: panel, width: 200, height: 100) {
    S.Text(text: "Hello", place: { x: 20, y: gap })
    // bottom centre of the panel, whatever its size
    S.Button(label: "Click me",
             place: { x: (panel.width - 120) / 2, y: panel.height - 24 - gap })
}
```

The name stands for the element's props: what it was given, plus the defaults of its props struct, as C++ sees them (`panel.width` is `SurfaceProps::width`). It is not measured layout, so a Text's width is not known through its id. Two elements in one block cannot share a name.

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
