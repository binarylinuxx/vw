---
title: Поверхности и роли
description: Сборка выбирает роль своей поверхности — layer-поверхность, окно или блокировка — вызовом одной из четырёх функций. Потом роль поменять нельзя.
---

| Вызов | Роль | Свойства |
| --- | --- | --- |
| `ui.surface(props)` | layer-поверхность | `SurfaceProps` |
| `ui.toplevel(props)` | окно | `ToplevelProps` |
| `ui.session_lock(props)` + `ui.lock_surface(props)` | блокировка сеанса, по поверхности на монитор | `SessionLockProps`, `LockSurfaceProps` |

Сборка, которая ничего не нарисовала, оставляет поверхность скрытой.

## SurfaceProps

| Поле | По умолчанию | Значение |
| --- | --- | --- |
| `width`, `height` | по содержимому | `0` заполняет эту ось; результат — в `vw::surface_size()` |
| `anchor` | `center` | края, битами `vw::anchor` |
| `layer` | `top` | `background`, `bottom`, `top`, `overlay` |
| `margin` | 0 | `{top, right, bottom, left}` |
| `exclusive_zone` | 0 | пиксели, резервируемые от привязанного края; `-1` игнорирует зоны других |
| `surface_namespace` | `"vw"` | для правил слоёв композитора |
| `output` | выбор композитора | монитор, на котором показывать, по имени (`"DP-1"`); другое имя позже переносит поверхность туда. [`App::per_output`](/ru/reference/app/#vwapp) заполняет его сам |
| `input_regions` | везде | прямоугольники `{x, y, w, h}`, принимающие ввод; пустой список — клики насквозь |
| `focus_grab`, `on_activated`, `on_dismiss` | выкл. | захват фокуса zwwm для попапов |
| `on_closed` | — | композитор закрыл поверхность |
| `refresh_interval` | 1 с | периодические пересборки, 0.008–3600 с |

## ToplevelProps

| Поле | По умолчанию | Значение |
| --- | --- | --- |
| `width`, `height` | по содержимому | начальный размер |
| `refresh_interval` | 1 с | периодические пересборки |
| `title`, `app_id` | `"vw"` | метаданные окна |
| `decorations` | `automatic` | `server` просит zwwm рисовать заголовок и рамки, `client` оставляет их вам; `vw::toplevel_server_decorated()` говорит, что выбрано |
| `on_close` | выход | окно закрыли |

## SessionLockProps и LockSurfaceProps

| Поле | Значение |
| --- | --- |
| `pam_service` | сервис PAM; `swaylock`, если настроен, иначе `login` |
| `pam_conf` | абсолютный путь к вашему собственному файлу PAM |
| `on_unlock` | сеанс разблокирован; без него приложение завершается |
| `width`, `height` (поверхность блокировки) | по умолчанию размер монитора |
