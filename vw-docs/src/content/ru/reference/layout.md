---
title: Раскладка
description: Раскладка происходит после сборки — сначала виджеты измеряются, потом строки, колонки, стопки и anchor-раскладки двигают их на места. Списки рисуют только видимое.
---

## Позиционирование и центрирование

Каждый виджет начинает в левом верхнем углу поверхности. Чтобы поставить его в другое место:

| Нужно | Используйте |
| --- | --- |
| по центру или у края поверхности | `ui.align(widget, vw::anchor::center)`, с битами `left`, `right`, `top`, `bottom` и отступом `{top, right, bottom, left}` |
| фиксированное место | `.place = {.x = 24, .y = 8}` в его свойствах |
| подвинуть после постройки | `ui.move(widget, dx, dy)` |
| выстроить вместе с другими | `row`, `column`, `grid` или `stack` ниже |

Панель с датой слева, часами посередине и сетью справа:

```cpp
const vw::Widget date = ui.text({.text = "Чт 12"});
const vw::Widget clock = ui.text({.text = "23:01"});
const vw::Widget net = ui.text({.text = "Проводное соединение 1"});
ui.align(date, vw::anchor::left, {0, 0, 0, 12});
ui.align(clock);                                    // по центру по обеим осям
ui.align(net, vw::anchor::right, {0, 12, 0, 0});
ui.surface({.width = 0, .height = 32, .anchor = vw::anchor::top});
```

`align` меряет относительно поверхности после configure, если она есть (здесь — панель во всю ширину), иначе относительно всего нарисованного до этого. Часы остаются посередине при любой ширине соседей — `justify = space_between` даёт это только при равных ширинах.

## row, column, stack

```cpp
const vw::Widget icon = ui.image({.icon = "audio-volume-high", .width = 20, .height = 20});
const vw::Widget label = ui.text({.text = "68%", .size = 14});
ui.row({icon, label}, {.gap = 8, .padding = 10, .radius = 18, .place = {.x = 12, .y = 6}});
```

`row` и `column` принимают детей и `LayoutProps` (`gap`, `padding`, необязательные фиксированные `width`/`height`, `justify`, `align`, `radius`, `place`, `input`). `stack` накладывает детей друг на друга в одной коробке. Каждый возвращает один виджет, покрывающий всё, — его можно снова раскладывать.

Когда фиксированная `width` (у row) или `height` (у column) оставляет место, `justify` расставляет детей вдоль оси:

| `justify` | Дети |
| --- | --- |
| `start` (по умолчанию), `center`, `end` | прижаты к этому краю, через `gap` |
| `space_between` | первый и последний у краёв, остальные равномерно между ними |
| `space_around` | одинаковое место вокруг каждого, у краёв — половина |
| `space_evenly` | одинаковое место между ними и у краёв |

`align` (`start`, `center` по умолчанию, `end`) ставит их поперёк оси. Собственный `place.anchor` ребёнка важнее: `top`/`bottom` в строке, `left`/`right` в колонке. Отступы учитываются.

```cpp
// Заголовок окна: название слева, кнопки справа.
ui.row({title, ui.row({minimise, close}, {.gap = 6})},
       {.width = 480, .padding = 12, .justify = vw::Justify::space_between});
```

## grid

`grid(children, {.columns, .gap, .row_gap, .padding, .width, .height, .align})` заполняет по `columns` ячеек в строке, слева направо. Колонки шириной с самого широкого ребёнка или делят фиксированную `width` поровну; каждая строка высотой с самого высокого ребёнка. `align` ставит ребёнка внутри ячейки по обеим осям; `row_gap` по умолчанию равен `gap`.

```cpp
std::vector<vw::Widget> tiles;
for (const auto& app : apps) tiles.push_back(app_tile(ui, app));
ui.grid(tiles, {.columns = 4, .gap = 16, .width = 560});
```

## anchor_layout

Для привязок в стиле QML внутри коробки:

```cpp
ui.anchor_layout({.width = 400, .height = 240, .children = {
  {.left = true, .top = true, .margin = {12, 0, 0, 12},
   .render = [&](const vw::AnchorBox& box) { return ui.text({.text = "Заголовок"}); }},
  {.left = true, .right = true, .bottom = true, .height = 48, .margin = {0, 12, 12, 12},
   .render = [&](const vw::AnchorBox& box) {
     return ui.rectangle({.width = box.width, .height = box.height, .radius = 12}); }},
}});
```

Привязка обоих краёв оси растягивает ребёнка; `min_*`/`max_*` ограничивают его. `render` получает вычисленную коробку и строит ребёнка, которого затем ставят на место.

## scrollbox

`scrollbox(children, {.id, .width, .height, .gap, .padding, .scroll_step})` складывает детей вертикально в обрезанное окно, которое прокручивается колёсиком или тачпадом. Смещение хранится под `id`; `vw::scroll_offset(id)` и `vw::set_scroll_offset(id, y)` читают и задают его.

## listview

Виртуализирован: `render(index)` вызывается только для строк внутри видимой области, так что список из тысяч элементов стоит одного экрана.

```cpp
ui.listview({.id = "apps", .width = 360, .height = 480, .item_height = 52, .gap = 4,
             .count = apps.size(),
             .render = [&](std::size_t i) -> std::optional<vw::Widget> { return row_for(apps[i]); },
             .keep_visible = selected});
```

- `row_height(i)` даёт строкам разную высоту (вызывается для каждой строки; держите её дешёвой).
- `keep_visible` прокручивает ровно настолько, чтобы эта строка была видна целиком, когда она меняется, — выделение с клавиатуры.
- Возврат `std::nullopt` пропускает строку.
