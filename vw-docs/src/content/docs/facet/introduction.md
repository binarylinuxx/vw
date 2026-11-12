---
title: What Facet is
description: A declarative view language for vw that compiles to C++ — no interpreter, no runtime tree, and clang checks every use of your state.
---

Facet describes views the way QML does, and compiles them ahead of time into the same C++ you would write by hand against `vw::Ui`. There is no interpreter and no object tree at runtime: a view is a function vw calls on every build.

```facet
let [ vw.ui as ui, vw.layout as layout ] in

view Counter(state, actions) {
    ui.Surface(anchor: top, layer: top) {
        layout.Row(gap: 12, padding: 12, bg: "#151a1e", radius: 16) {
            ui.Text(text: "Clicked " + state.clicks + " times", color: "#dfe6ee")
            ui.Rectangle(width: 32, height: 32, radius: 16, color: "#a7cbe7",
                         input: { on_click: () => actions.click() })
        }
    }
}
```

```cpp
#include "counter.facet.hpp"   // written by vw-facetc

struct State { int clicks = 0; };
struct Actions { State* state; void click() { ++state->clicks; } };

int main() {
  State state;
  Actions actions{&state};
  vw::App app([&](vw::Ui& ui) { views::Counter(ui, state, actions); });
  return app.run();
}
```

## What that buys

- **Your types, checked.** A view is a function template over its parameters: `state.clicks` must exist on your `State`, or clang says so — at the `.facet` line, through `#line` directives.
- **The design system stays in charge.** Facet sets only the properties an element has; a typo is an error with the line and a caret, not a silently ignored key.
- **C++ when you need it.** Expressions are C++ calls, handlers call your actions, and [`cpp { }` blocks](/facet/cpp/) or C++ components cover the rest.
- **Nothing extra to ship.** The output is plain vw; a shell built from Facet is the same single binary.

Facet is optional: it is built only with `-DVW_WITH_FACET=ON`. See [Build and tooling](/facet/setup/).
