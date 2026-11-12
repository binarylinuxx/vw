---
title: Анимация
description: Плавное изменение значения через animate(), привязанное к его месту в файле и циклам вокруг.
---

```facet
ui.Rectangle(width: animate(360 * state.progress / 100, 200), height: 6, color: "#a7cbe7")
```

`animate(target, milliseconds[, easing])` возвращает значение, движущееся к `target`; vw запрашивает кадры, пока оно не дойдёт. Работает везде, где можно число. Кривые: `linear`, `in_out_cubic`, `out_cubic` (по умолчанию), `out_quint`, `out_expo`, `out_back`, `spring`.

Каждый `animate` привязан к своей строке и колонке плюс `key` каждого окружающего `for`, так что каждый элемент списка движется сам по себе:

```facet
for task in state.tasks key task.id {
    ui.Rectangle(width: animate(task.done ? 0 : 280, 160, out_back), height: 36)
}
```

Форма через свойство анимирует одно свойство элемента:

```facet
ui.Rectangle(width: 368 * state.progress / 100, animate: { property: "width", duration: 200 })
```
