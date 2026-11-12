---
title: Вход (greetd)
description: Клиент greetd для экрана входа — вопросы, ответы и запуск сеанса, плюс пользователи и сеансы, которые можно предложить.
---

[greetd](https://sr.ht/~kennylevinsen/greetd/) проверяет пользователя и запускает сеансы; greeter только задаёт вопросы. `vw/greetd.hpp` — это сторона greeter'а: greetd запускает вашу программу с заданным `$GREETD_SOCK`, а ответы приходят на fd событий, так что экран остаётся отзывчивым, пока PAM думает.

| Функция | Значение |
| --- | --- |
| `available()` | эту программу запустил greetd |
| `begin(username)` | начинает вход, отменяя уже идущий |
| `state()` | `stage`, `prompt`, `message`, `username`, `denied` |
| `answer(text)` | отвечает на текущий вопрос; вопросы `info` и `error` ответа не требуют |
| `start(command, env)` | запускает прошедший проверку сеанс, с дополнительными переменными `KEY=value` |
| `cancel()` | бросает вход, возврат в `idle` |
| `event_fd()`, `drain_events()` | для `app.on_event`: greetd ответил |
| `users()` | учётные записи, которые могут войти: uid из диапазона входа и настоящая оболочка |
| `sessions()` | установленные сеансы Wayland, потом X: `name`, `command`, `file`, `wayland` |

## Порядок входа

`stage` проходит `idle` → `waiting` → `prompting` (greetd что-то спрашивает) → … → `authenticated` → `started`. Каждый ответ возвращает в `waiting`, пока greetd не ответит. Вход может задать несколько вопросов: пароль, потом одноразовый код.

| `prompt` | Что показать |
| --- | --- |
| `secret` | скрытое поле: пароль |
| `visible` | обычное поле: имя пользователя, код |
| `info`, `error` | сообщение и способ продолжить (`answer()` без текста) |

`failed` завершает вход: `message` говорит почему, а `denied` отличает неверные данные от ошибки в greetd. Чтобы попробовать снова — снова `begin()`; неудавшийся сеанс vw отменит за вас.

```cpp
app.on_event(vw::greetd::event_fd(), vw::greetd::drain_events);

// в функции сборки
const auto login = vw::greetd::state();
if (login.stage == vw::greetd::Stage::authenticated)
  vw::greetd::start(chosen_session.command);
if (login.stage == vw::greetd::Stage::started) std::exit(0);  // дальше работает greetd
```

Поле пароля — обычный `ui.text_field` с `password = true`; при отправке — `vw::greetd::answer(text)`.

## Запуск

greetd запускает greeter внутри собственного композитора, например в `/etc/greetd/config.toml`:

```toml
[default_session]
command = "cage -s -- my-greeter"
user = "greeter"
```

Чтобы разрабатывать, не выходя из сеанса, `fakegreet` из greetd запускает greeter против ненастоящего демона: `fakegreet ./my-greeter`. Он спрашивает пароль (`password`), а затем `7 + 2` (`9`) для пользователя `user`.
