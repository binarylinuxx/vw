---
title: Приложения и трей
description: Desktop-файлы для лаунчеров и StatusNotifierItem для трея с меню.
---

## Приложения — `vw/applications.hpp`

```cpp
for (const auto& app : vw::applications::search(query, 40))
  row(app.display_name, app.icon_path);
vw::applications::launch(app.id);
```

`search(query, limit)` возвращает видимые приложения, лучшее совпадение первым — точное имя, начало имени, начало слова, подстрока имени, затем другие поля и ключевые слова; пустой запрос перечисляет всё по отображаемому имени, а лимит 0 — без ограничения. `Application` содержит `id`, `name`, `display_name`, `generic_name`, `description`, `executable`, `commandline`, `icon`, `icon_path` (найденный путь), `terminal`, `categories`, `keywords` и `startup_wm_class`. `launch(id)` запускает приложение по desktop id (`firefox.desktop`) и бросает исключение при ошибке.

## Трей — `vw/tray.hpp`

`start()` предоставляет `org.kde.StatusNotifierWatcher` и принимает элементы; дальше:

| Функция | Значение |
| --- | --- |
| `items()` | `id`, `title`, `status` (`passive`, `active`, `needs_attention`), `icon_path`/`icon`, подсказка, `has_menu` |
| `activate(id, x, y)`, `secondary_activate(…)` | левый и средний клик |
| `context_menu(id, x, y)` | просит элемент показать своё собственное меню |
| `scroll(id, delta, horizontal)` | колёсико |
| `menu(id)` | дерево DBusMenu: `label`, `icon`, `enabled`, `separator`, флажок/радио `toggle`, `children` |
| `activate_menu(id, entry)` | выполняет пункт меню |

Рисуйте `menu(id)` сами — тогда меню будут в одном стиле с остальной оболочкой.
