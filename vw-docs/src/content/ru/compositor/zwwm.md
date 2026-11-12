---
title: Теги, снимки, действия
description: Протоколы manager и tags композитора zwwm из vw/zwwm.hpp — на отдельном соединении, с fd событий для цикла.
---

`vw/zwwm.hpp` общается с zwwm через собственное соединение Wayland. Вызовы бросают исключение, если нужного протокола нет; `available()` говорит, есть ли zwwm вообще.

| Функция | Значение |
| --- | --- |
| `available()` | zwwm запущен |
| `tags()`, `active_tags()` | теги с `id`, `name`, `active` |
| `set_active_tags(ids)` | показывает теги на мониторе с фокусом |
| `snapshot(type)` | JSON для `status`, `outputs`, `toplevels`, `output_tags`, `layers`, `cameras`, `keyboard`, `live` |
| `dispatch(action, argument)` | выполняет действие композитора; возвращает `{success, message}` |
| `reload()`, `set_cursor(theme, size)`, `rebuild_switch_shaders()` | управление композитором |
| `last_event()` | самое свежее событие подписки |
| `event_fd()`, `drain_events()` | для `app.on_event`: читаем, когда меняется состояние композитора |

## Пересборка по событиям

zwwm шлёт события много раз в секунду; сравнивайте то, что рисуете (теги, окно с фокусом), прежде чем решать, пересобирать ли. Снимок — это запрос к композитору и ответ: делайте один на изменение, а не один на кадр.

```cpp
app.on_event(vw::zwwm::event_fd(), [] {
  if (!vw::zwwm::drain_events()) return false;
  static std::vector<std::uint32_t> last;
  auto now = vw::zwwm::active_tags();
  if (now == last) return false;
  last = std::move(now);
  return true;  // пересобрать
});
```

## Биты состояния окна

`snapshot(Snapshot::toplevels)` сообщает `state` каждого окна битами: `1` показано, `2` в фокусе, `4` плавающее, `8` на весь экран, `16` модальный диалог, `32` скрыто. Оболочка, которая прячется от полноэкранных окон, проверяет бит 8 у окна с фокусом.
