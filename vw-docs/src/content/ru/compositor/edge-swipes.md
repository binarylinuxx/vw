---
title: Свайпы от края
description: Касания, начинающиеся у края монитора, — их распознаёт zwwm и передаёт вашей оболочке.
---

zwwm распознаёт касания, которые начинаются у края монитора, и не отдаёт их другим клиентам; оболочка только анимирует и решает, что делает свайп.

| Функция | Значение |
| --- | --- |
| `watch_edge_swipes(edges)` | следить за свайпами от `Edge::top`, `bottom`, `left`, `right` (через ИЛИ); `0` перестаёт |
| `take_edge_swipes()` | свайпы с прошлого вызова: `phase` (`began`, `progress`, `ended`), `edge`, `distance`, `lateral`, `velocity`, `cancelled` |

Свайпы приходят через `vw::zwwm::event_fd()`, как и все остальные события zwwm. `distance` — насколько палец сдвинулся внутрь экрана; тяните им панель, пока свайп идёт, а на `ended` решайте по `distance` и `velocity`:

```cpp
vw::zwwm::watch_edge_swipes(vw::zwwm::left);
app.on_event(vw::zwwm::event_fd(), [] {
  const bool changed = vw::zwwm::drain_events();
  const auto swipes = vw::zwwm::take_edge_swipes();
  for (const auto& swipe : swipes) {
    sidebar_offset = swipe.phase == vw::zwwm::EdgeSwipe::Phase::ended ? 0 : swipe.distance;
    if (swipe.phase == vw::zwwm::EdgeSwipe::Phase::ended && !swipe.cancelled)
      sidebar_open = swipe.distance > 120 || swipe.velocity > 600;
  }
  return changed || !swipes.empty();  // свайп двигает панель: пересобрать
});
```

На композиторе без этого протокола `watch_edge_swipes` бросает исключение; поймайте его и работайте без свайпов.
