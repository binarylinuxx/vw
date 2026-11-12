---
title: Views and imports
description: Imports, views and elements — the structure of a .facet file.
---

## Imports

```facet
let [ vw.ui as ui, vw.layout as layout ] in
```

A file starts by naming its modules. `vw.ui` holds surfaces, shapes, text, images, fields and [controls](/facet/controls/); `vw.layout` holds rows, columns, grids and lists. Any other module is a [C++ component module](/facet/cpp/#component-modules), such as a design system.

## Views

```facet
view Settings(state, actions) { … }
```

A view becomes a C++ function template: `views::Settings(ui, state, actions)`. Its parameters can have any type; clang checks how the view uses them. A file holds as many views as you like.

- A view whose body is one `ui.Surface(…)` is a whole surface: call it from `vw::App`'s build function.
- Any other view returns a `vw::Widget`, to lay out from C++ or another view's [component](/facet/cpp/#native-widgets).

## Elements

```facet
layout.Column(gap: 8, padding: 16) {
    ui.Text(text: "Title", size: 20, weight: bold)
    ui.Text("Shorter: the first argument may be positional")
}
```

An element is `module.Name(arguments) { children }`. Arguments are named (`size: 20`); `Text`'s `text` and `Image`'s `path` may also come first, unnamed. Children go in braces. Every property is checked: an unknown one is an error that lists what the element has.

The [element reference](/facet/elements/) lists each element and its properties.
