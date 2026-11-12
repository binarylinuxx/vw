---
title: Expressions and handlers
description: Values, colors, enum names, operators, conditionals, lambdas and the three ways to write a handler.
---

## Values

| Write | Means |
| --- | --- |
| `12`, `0.5`, `"text"`, `true`, `null` | numbers, text, booleans, `std::nullopt` |
| `"#20272c"`, `"#a7cbe7f0"`, `"transparent"` | colors; a C++ `vw::Color` in scope works too (`color: accent`) |
| `bold`, `space-between`, `contain`, `overlay` | enum names, checked against the property |
| `"100%"` | a [relative size](/facet/layout/#relative-sizes) |
| `[a, b, c]` | a list |
| `state.items`, `items[0]`, `format_bytes(n)` | members, indexing and calls: plain C++ |
| `items.length` | the size of a list |
| `screen.width`, `screen.height`, `screen.name` | the monitor being drawn on (also `x`, `y`, `scale`, `refresh` in Hz); with [`App::per_output`](/guides/shells/#one-bar-per-monitor), each bar's own |

## Operators

Arithmetic (`+ - * / %`), comparisons, `&&`, `||` and `!` work as in C++. `+` with text on either side joins: `"CPU " + state.cpu + "%"`.

Two spellings of a conditional, as you prefer:

```facet
color: state.active ? "#a7cbe7" : "#20272c"
color: if state.active then "#a7cbe7" else "#20272c"
```

Both work inside properties of any type, including colors and enum names.

## Handlers

A property that takes an event (`on_click`, `on_change`, `on_submit`, or a component's `onPress`) accepts three forms:

```facet
on_click: () => actions.open(app.id)          // a lambda
on_change: actions.search                     // a function: gets what vw passes
onPress: actions.select(tab.id)               // a call: runs on the event, not now
```

Lambdas take parameters (`(value) => actions.set(value)`) and may hold several statements:

```facet
on_click: () => {
    let name = app.name
    actions.remember(name)
    actions.open(app.id)
}
```

A handler copies the locals it uses (loop variables included) and reaches the view's parameters as the live objects, so `actions` and `state` are never copies. For a body written in C++, see [`() => cpp { … }`](/facet/cpp/#c-blocks).
