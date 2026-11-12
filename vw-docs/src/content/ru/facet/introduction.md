---
title: Что такое Facet
description: Декларативный язык описания интерфейса для vw, который компилируется в C++, — без интерпретатора, без дерева во время работы, и clang проверяет каждое обращение к вашему состоянию.
---

Facet описывает интерфейс так же, как QML, и заранее компилирует его в тот же C++, который вы написали бы руками поверх `vw::Ui`. Интерпретатора и дерева объектов во время работы нет: view — это функция, которую vw вызывает при каждой сборке.

```facet
let [ vw.ui as ui, vw.layout as layout ] in

view Counter(state, actions) {
    ui.Surface(anchor: top, layer: top) {
        layout.Row(gap: 12, padding: 12, bg: "#151a1e", radius: 16) {
            ui.Text(text: "Нажато " + state.clicks + " раз", color: "#dfe6ee")
            ui.Rectangle(width: 32, height: 32, radius: 16, color: "#a7cbe7",
                         input: { on_click: () => actions.click() })
        }
    }
}
```

```cpp
#include "counter.facet.hpp"   // написан vw-facetc

struct State { int clicks = 0; };
struct Actions { State* state; void click() { ++state->clicks; } };

int main() {
  State state;
  Actions actions{&state};
  vw::App app([&](vw::Ui& ui) { views::Counter(ui, state, actions); });
  return app.run();
}
```

## Что это даёт

- **Ваши типы, проверенные.** View — шаблон функции над своими параметрами: `state.clicks` должно существовать в вашем `State`, иначе clang скажет об этом — на строке `.facet`, через директивы `#line`.
- **Дизайн-система остаётся главной.** Facet задаёт только те свойства, которые у элемента есть; опечатка — это ошибка со строкой и указателем, а не молча проигнорированный ключ.
- **C++, когда он нужен.** Выражения — это вызовы C++, обработчики вызывают ваши actions, а [блоки `cpp { }`](/ru/facet/cpp/) или компоненты на C++ покрывают остальное.
- **Ничего лишнего в поставке.** На выходе обычный vw; оболочка на Facet — тот же единственный бинарник.

Facet необязателен: он собирается только с `-DVW_WITH_FACET=ON`. См. [Сборка и инструменты](/ru/facet/setup/).
