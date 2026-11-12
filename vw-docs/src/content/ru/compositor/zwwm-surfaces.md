---
title: Захват фокуса и декорации
description: Дополнения zwwm для поверхностей — попапы, которые знают, когда пользователь кликнул в другом месте, и окна, которые декорирует композитор.
---

## Захват фокуса

С zwwm layer-поверхность может попросить сообщить ей, когда пользователь кликнет за её пределами: задайте `focus_grab = true` в `SurfaceProps`. `on_activated` вызывается, когда захват начался; `on_dismiss` получает `FocusEvent`, у которого `reason()` — `outside`, `unmapped`, `replaced`, `locked` или `released`: там и закрывайте попап.

```cpp
ui.surface({.layer = vw::Layer::overlay, .focus_grab = menu_open,
            .on_dismiss = [](const vw::FocusEvent&) { menu_open = false; }});
```

`vw::focus_grab_state()` говорит, доступны ли захваты и активен ли сейчас какой-то. Другие композиторы игнорируют `focus_grab`.

## Декорации

`ToplevelProps::decorations` просит zwwm рисовать заголовок и рамки окна (`server`) или оставить их окну (`client`); `automatic` принимает выбор композитора. zwwm отвечает, что выбрал, и может потом передумать: читайте `vw::toplevel_server_decorated()` и не рисуйте свой заголовок, когда оно истинно. Окно пересобирается, когда ответ меняется.

```cpp
ui.toplevel({.title = "Notes", .app_id = "notes", .decorations = vw::Decorations::server});
if (!vw::toplevel_server_decorated()) draw_titlebar(ui);
```
