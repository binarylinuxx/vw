---
title: C++ в Facet
description: Заголовки, блоки cpp { }, нативные виджеты и модули компонентов на C++, например дизайн-система.
---

## Заголовки

```facet
include "inbox.hpp"          // #include "inbox.hpp"
include "<format>"           // #include <format>
```

`include` пишется до или после импортов. Функции и типы из заголовка можно использовать в любом выражении.

## Блоки C++

```facet
cpp {                                   // уровень файла: помощники, типы, константы
    inline std::string unread_label(int n) { return std::format("{} unread", n); }
}

view Inbox(state, actions) {
    cpp {                               // внутри view: инструкции, там, где стоят
        int unread = 0;
        for (const auto& m : state.messages) unread += m.unread ? 1 : 0;
    }
    ui.Text(text: unread_label(unread))

    ui.Rectangle(input: { on_click: () => cpp {   // тело обработчика
        actions.open(state.messages.front());
    } })
}
```

- Блок уровня файла выводится перед всеми view.
- Внутри view в области видимости `ui`, параметры view и переменные циклов, а то, что объявил блок, может использовать Facet после него.
- Как тело обработчика блок копирует используемые локальные переменные и видит `state` и `actions` как живые объекты.
- facetc копирует блоки как есть (скобки внутри строк и комментариев их не закрывают); clang их проверяет, и его ошибки указывают на строку `.facet`.

Оставляйте блоки для маленьких кусочков логики; целые виджеты читаются лучше как нативные виджеты или компоненты.

## Нативные виджеты

```facet
Native(name: CpuGraph, width: "100%", height: 120, props: { data: state.history })
```

вызывает ваш C++ `vw::Widget CpuGraph(vw::Ui&, const CpuGraphProps&)`, выставляя каждый аргумент и каждое поле `props` в `CpuGraphProps`.

## Модули компонентов

Импорт модуля, который не является vw, превращает его элементы в вызовы C++:

```facet
let [ vw.ui as ui, JES.spectralium as S ] in
…
S.Text(text: state.title, role: heading, tone: muted)
S.Card(padding: normal) { … }
```

`S.Text(…)` вызывает `JES::spectralium::Text(ui, TextProps)`. Если есть дети, их передаёт третий аргумент, и строятся они тогда, когда компонент попросит: сначала нарисуйте фон, потом вызовите `children(ui)` — и дети лягут поверх него.

```cpp
vw::Widget Card(vw::Ui& ui, const CardProps& props, const facet::Children& children) {
  const vw::Widget back = ui.rectangle({.width = props.width, .height = props.height, .radius = 16});
  for (const vw::Widget& child : children(ui)) ui.move(child, 12, 12);  // внутрь карточки
  vw::Widget card = back;
  card.end = ui.scene().rectangles.size();  // карточка покрывает и своих детей
  return card;
}
```

Компонент может вместо этого принимать `std::span<const vw::Widget>` или `std::vector<vw::Widget>`; тогда дети строятся до того, как выполнится его тело, и оказываются под всем, что он рисует.

В свойствах компонента:

- простое имя, которое не является переменной, — это значение перечисления типа поля: `role: heading` означает `decltype(props.role)::heading`
- `"#rrggbb"` — это `vw::Color`, `"N%"` — относительный размер, объект задаёт поля по одному (`config: { refresh_rate: 1000 }`)
- первый безымянный аргумент задаёт `value`; поля с именами событий (`onPress`, `on_click`) принимают обработчики; `flex` передаётся дальше для собственной раскладки компонента

`vw-facetc --module JES.spectralium` (в CMake: `MODULE`) позволяет писать в файле `Text(…)` вместо `S.Text(…)`.
