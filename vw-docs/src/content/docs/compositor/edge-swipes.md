---
title: Edge swipes
description: Touch swipes that start at an output edge, recognised by zwwm and delivered to your shell.
---

zwwm recognises touches that start at an output edge and keeps them away from other clients; the shell only animates and decides what a swipe does.

| Function | Meaning |
| --- | --- |
| `watch_edge_swipes(edges)` | follow swipes from `Edge::top`, `bottom`, `left`, `right` (ORed); `0` stops |
| `take_edge_swipes()` | swipes since the last call: `phase` (`began`, `progress`, `ended`), `edge`, `distance`, `lateral`, `velocity`, `cancelled` |

Swipes arrive through `vw::zwwm::event_fd()` like every other zwwm event. `distance` is how far the finger has moved inwards; drag a panel with it while the swipe is in progress, then decide on `ended` from `distance` and `velocity`:

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
  return changed || !swipes.empty();  // a swipe moves the panel: rebuild
});
```

On a compositor without the protocol, `watch_edge_swipes` throws; catch it and carry on without swipes.
