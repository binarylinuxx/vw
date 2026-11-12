---
title: Окна приложений
description: Та же функция сборки становится окном xdg-toplevel — с заголовком, app id, изменением размера, текстовыми полями и буфером обмена.
---

Вызовите `ui.toplevel(props)` вместо `ui.surface(props)`:

```cpp
vw::App app([&](vw::Ui& ui) {
  const vw::Size size = vw::toplevel_size();  // configure композитора важнее
  ui.rectangle({.width = (float)size.width, .height = (float)size.height,
                .color = vw::rgb(0x101419)});
  ui.text_field({.id = "query", .placeholder = "Поиск…", .width = 320, .autofocus = true,
                 .on_submit = [](const std::string& text) { run(text); },
                 .place = {.x = 24, .y = 24}});
  ui.toplevel({.width = 640, .height = 420, .title = "Finder", .app_id = "my.finder",
               .on_close = [] { /* сохранить и дать приложению завершиться */ }});
});
```

- `width` и `height` — начальный размер; после того как композитор настроил окно, правду говорит `vw::toplevel_size()`. Раскладывайте относительно него, и изменение размера просто заработает.
- Без `on_close` закрытие окна завершает приложение.
- `vw::request_toplevel_size(w, h)` просит новый размер; `vw::request_toplevel_close()` закрывает окно из кода.
- Роль поверхности фиксирована: одна поверхность не может при перезагрузке переключиться между toplevel, layer и lock.

## Текстовые поля

`text_field` — полноценный однострочный редактор: курсор, выделение перетаскиванием, двойной клик выделяет слово, тройной — всё, Home/End, прыжки и удаление по словам с Ctrl, Ctrl+A/C/X/V через системный буфер обмена и маскировка пароля. Значение хранится под `id`; `value` задаёт его только в первый раз. `vw::reset_text_field(id)` забывает его (например, когда закрывается диалог).

| Свойство | Значение |
| --- | --- |
| `on_change`, `on_submit` | вызываются с новым текстом / по Enter |
| `password` | показывает точки, никогда не копирует |
| `autofocus` | получает фокус сразу при появлении |
| `max_length` | в байтах, по умолчанию 4096 |
| `focus_color`, `border_color`, `caret_color`, `selection_color` | оформление |

## Буфер обмена

`vw::set_clipboard(text)` и `vw::clipboard_text()` используют поверхность, получившую ввод последней (Wayland требует serial ввода), через `wlr-data-control`, когда он доступен.
