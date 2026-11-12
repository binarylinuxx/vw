---
title: C++ in Facet
description: Headers, cpp { } blocks, native widgets and C++ component modules such as a design system.
---

## Headers

```facet
include "inbox.hpp"          // #include "inbox.hpp"
include "<format>"           // #include <format>
```

`include` goes before or after the imports. Functions and types from the header are usable in any expression.

## C++ blocks

```facet
cpp {                                   // file level: helpers, types, constants
    inline std::string unread_label(int n) { return std::format("{} unread", n); }
}

view Inbox(state, actions) {
    cpp {                               // in a view: statements, where they stand
        int unread = 0;
        for (const auto& m : state.messages) unread += m.unread ? 1 : 0;
    }
    ui.Text(text: unread_label(unread))

    ui.Rectangle(input: { on_click: () => cpp {   // a handler body
        actions.open(state.messages.front());
    } })
}
```

- A file-level block comes out before every view.
- In a view, `ui`, the view's parameters and loop variables are in scope, and what the block declares is usable by the Facet after it.
- As a handler body, the block copies the locals it uses and sees `state` and `actions` as the live objects.
- facetc copies blocks as written (braces inside strings and comments do not end them); clang checks them, and its errors point at the `.facet` line.

Keep blocks for small pieces of logic; whole widgets read better as native widgets or components.

## Native widgets

```facet
Native(name: CpuGraph, width: "100%", height: 120, props: { data: state.history })
```

calls your C++ `vw::Widget CpuGraph(vw::Ui&, const CpuGraphProps&)`, setting each argument, and each field of `props`, on a `CpuGraphProps`.

## Component modules

Importing a module that is not vw makes its elements C++ calls:

```facet
let [ vw.ui as ui, JES.spectralium as S ] in
…
S.Text(text: state.title, role: heading, tone: muted)
S.Card(padding: normal) { … }
```

`S.Text(…)` calls `JES::spectralium::Text(ui, TextProps)`; with children, they come as a third argument. In a component's props:

- a bare name that is not a variable is an enum value of the field's type: `role: heading` is `decltype(props.role)::heading`
- `"#rrggbb"` is a `vw::Color`, `"N%"` a relative size, an object sets fields one by one (`config: { refresh_rate: 1000 }`)
- a first unnamed argument sets `value`; fields named like events (`onPress`, `on_click`) take handlers; `flex` is passed on for the component's own layout

`vw-facetc --module JES.spectralium` (CMake: `MODULE`) lets a file write `Text(…)` for `S.Text(…)`.
