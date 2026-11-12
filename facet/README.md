# Facet

Facet is a declarative view language for vw. `vw-facetc` compiles a `.facet`
file into a C++ header: each view becomes a function template over its
parameters, so clang type-checks every use of state and actions, and `#line`
directives point C++ errors back at the `.facet` line. There is no runtime
interpreter.

Facet is optional: build vw with `-DVW_WITH_FACET=ON` (Nix: `vw-facet`, or
`vw.lib.mkShell { facet = true; … }`). Without it, nothing of Facet is built
or installed.

```cmake
find_package(vw REQUIRED)            # vw built with VW_WITH_FACET
facet_compile(OUTPUT views.facet.hpp SOURCE views.facet)
add_executable(my-shell main.cpp views.facet.hpp)
target_link_libraries(my-shell PRIVATE vw::vw)
```

`facet_compile` takes `NAMESPACE` (default `views`) and `MODULE` (see
`--module` below). The generated header includes `<vw/facet.hpp>`, Facet's
small runtime.

Examples, built with `VW_BUILD_EXAMPLES`: `examples/launcher` (a launcher
over installed apps) and `examples/cpp-blocks` (C++ inside Facet; its check
clicks a message through a C++ handler). `editors/micro/facet.yaml` is syntax
highlighting for micro: copy it to `~/.config/micro/syntax/`.

## What compiles today

- `let [ vw.ui as ui, vw.layout as layout, JES.spectralium as S ] in` imports
- `view Name(state, actions) { … }`; a view whose body is a `ui.Surface` is a
  whole surface, any other view returns a `vw::Widget`
- vw elements: `ui.Surface`, `ui.Rectangle` (with content on top), `ui.Text`,
  `ui.Image` (an empty path keeps its size and draws nothing), `ui.TextField`,
  with `padding` on Text and Image, and `align: center_left` (or `left`,
  `top_right`, …) placing a Text in its parent; `start`/`center`/`end` align
  its lines
  `layout.Row`, `layout.Column`, `layout.Grid`, `layout.ListView`
- controls built on vw (`<vw/facet.hpp>`): `ui.Toggle`, `ui.Slider`,
  `ui.Dropdown`, `ui.Spinner`
- layout backgrounds: `bg`/`color`, `radius`, `border_width`, `border_color` on
  `Row`, `Column` and `Grid`
- sizes: `width: "100%"` of the content box around the element (a parent's
  width or height, less padding); `flex: N` shares a Row's or Column's spare
  room, and a Row or Column with flex children spans its parent
- `layout.ListView(count:) { (index) => { … } }`: width defaults to the space
  around it, a missing `item_height` is measured from the first row, and a
  missing `height` shows every row instead of scrolling
- animation: `width: animate(368 * progress, 200[, out_back])`, or
  `animate: { property: "width", duration: 200 }`; each tween is keyed by its
  place in the file and the `key` of the loops around it
- components from C++: an imported module that is not vw (`S.Text(role:
  heading)`) calls `JES::spectralium::Text(ui, TextProps)`, with children passed
  as a third argument; `Native(name: Graph, …)` calls `Graph(ui, GraphProps)`.
  In their props, a bare name that is not a variable is an enum value of the
  field's type (`role: heading`), `"#rrggbb"` is a `vw::Color`, an object sets
  fields one by one (`config: { refresh_rate: 1000 }`), a first unnamed
  argument sets `value`, fields named like events (`onPress`, `on_click`) take
  handlers, and `flex` is passed on for the component's own layout
- `vw-facetc --module JES.spectralium` (CMake: `MODULE`) takes elements written
  without a module (`Surface(…)`) from that module
- handlers: a lambda, a function (`on_change: actions.update`), or a call that
  runs on the event (`onPress: actions.select(tab.id)`)
- `for x in list [key …]`, `if … { } else { }`, `let x = …`
- expressions: arithmetic, comparisons, `&&`, `||`, `!`, `a.b`, `a[i]`, calls,
  `cond ? a : b` and `if c then a else b`, `[a, b]` lists, `null`,
  `list.length`, `+` joining text with anything
- lambdas: `() => expr`, `(x) => expr`, `() => { let a = …; actions.f(a) }`;
  they copy what they use and reach the view's parameters by reference
- colors `"#rrggbb"`, `"#rrggbbaa"`, `"transparent"` or a C++ `vw::Color` in
  scope (`color: accent`); enum names such as `anchor: right`,
  `justify: space-between`, `weight: bold`, `fit: contain`

## C++ in Facet

```facet
include "inbox.hpp"          // #include "inbox.hpp"; "<format>" for <format>

cpp {                        // file level: helpers, types, constants
  inline std::string unread_label(int n) { return std::format("{} unread", n); }
}

view Inbox(state, actions) {
  cpp { int unread = 0; for (const auto& m : state.messages) unread += m.unread; }
  ui.Text(text: unread_label(unread))          // `unread` comes from the block
  ui.Rectangle(input: { on_click: () => cpp {  // a handler written in C++
    actions.open(state.messages.front());
  } })
}
```

- `include` goes before or after the imports; file-level `cpp { }` blocks come
  out before every view.
- In a view, a block's statements run where they stand, with `ui`, the view's
  parameters and loop variables in scope; what it declares is usable after it.
- `() => cpp { … }` is a handler body: it copies the locals it uses and sees
  `state` and `actions` as the live objects.
- Blocks are C++ as written: facetc does not check them, but clang does, and
  its errors point at the `.facet` line. Braces inside strings, character
  literals and comments do not end a block.
- Prefer `Native` or a component module for whole widgets, and keep blocks for
  small pieces of logic.

Unknown elements and properties are errors at the `.facet` line, and C++
errors in generated code point there too. The generated C++ builds with no
warnings under `-Wall -Wextra`.

