---
title: Раскладка и размеры
description: Строки, колонки, сетки и списки; justify и align; фоны; относительные размеры и flex.
---

## Строки, колонки, сетки

```facet
layout.Row(gap: 8, padding: 12, width: 480, justify: space-between, align: center) { … }
layout.Grid(columns: 4, gap: 16, width: 560) { … }
```

`justify` расставляет детей вдоль оси, когда строка или колонка длиннее их: `start`, `center`, `end`, `space-between`, `space-around`, `space-evenly`. `align` ставит их поперёк: `start`, `center`, `end`. Сетка заполняет по `columns` ячеек в строке; её колонки делят фиксированную `width` поровну.

## Фоны

```facet
layout.Column(gap: 8, padding: 16, bg: "#151a1e", radius: 16) { … }
```

`bg` (или `color`), `radius`, `border_width` и `border_color` у строки, колонки или сетки рисуют скруглённый фон по размеру содержимого. `ui.Rectangle` с детьми кладёт их поверх себя, по центру.

## Относительные размеры

```facet
layout.Column(width: 600, padding: 20) {
    ui.TextField(width: "100%")   // 560: ширина колонки минус её отступы
}
```

`"N%"` — доля области содержимого вокруг элемента: ширины или высоты ближайшего родителя за вычетом его отступов. Родитель без такого размера передаёт размер своего родителя. Если ни у одного предка его нет, Facet скажет об этом на этой строке.

## flex

```facet
layout.Row(gap: 16) {
    ui.Rectangle(flex: 1, height: 80) { … }
    ui.Rectangle(flex: 2, height: 80) { … }
}
```

`flex` делит пропорционально то, что остальные дети оставили от ширины строки (или высоты колонки). Строка или колонка с flex-детьми растягивается на весь родитель. Flex-дети пишутся прямо внутри неё, не в `for` или `if`.

## Списки

```facet
layout.ListView(count: state.files.length, height: 300) {
    (index) => {
        let file = state.files[index]
        ui.Text(text: file.name)
    }
}
```

`ListView` рисует только видимые строки и прокручивает остальные. Его ширина по умолчанию — пространство вокруг; без `item_height` измеряется первая строка; без `height` он показывает все строки, а не прокручивает.
