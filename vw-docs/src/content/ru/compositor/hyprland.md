---
title: Hyprland
description: IPC Hyprland из vw/hyprland.hpp — запросы, диспатчи и сокет событий.
---

На Hyprland vw читает состояние композитора через собственные сокеты Hyprland, а не через протоколы Wayland.

| Функция | Значение |
| --- | --- |
| `available()` | `HYPRLAND_INSTANCE_SIGNATURE` указывает на запущенный экземпляр |
| `query(endpoint)` | JSON из `j/<endpoint>`: `activeworkspace`, `clients`, `monitors`, `devices`… |
| `request(command)` | сырой запрос в сокет |
| `dispatch(args)`, `keyword(args)` | `{ok, response}` |
| `last_event()` | последнее событие socket2 `{name, data, sequence}` |
| `event_fd()`, `drain_events()` | для `app.on_event` |

Поток событий сам переподключается, когда Hyprland перезапускается. Запросы блокирующие: кэшируйте результаты и сбрасывайте их в обработчике событий.

Layer-поверхности, экраны блокировки и захват мониторов работают на Hyprland через стандартные протоколы со страницы [Композиторы и протоколы](/ru/compositor/overview/).
