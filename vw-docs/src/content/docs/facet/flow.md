---
title: Loops, conditions, let
description: Repeating elements, choosing between them, and naming values.
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
