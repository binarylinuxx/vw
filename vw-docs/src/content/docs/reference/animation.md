---
title: Animation
description: The vw/animation.hpp API in full. The guide on motion and gestures shows it in use.
---

```cpp
double vw::animate(std::string_view id, double target, const vw::AnimationOptions& options = {});
std::optional<vw::Easing> vw::parse_easing(std::string_view name);
void vw::request_animation_continuation();
bool vw::animations_active();
void vw::begin_animation_frame();
```

## AnimationOptions

| Field | Default | Meaning |
| --- | --- | --- |
| `duration` | 0.28 | seconds; `0` jumps to the target |
| `initial` | target | starting value the first time `id` is seen |
| `easing` | `out_cubic` | see below |
| `bezier` | `{0.25, 0.1, 0.25, 1}` | control points for `Easing::bezier` |
| `mass`, `stiffness`, `damping` | 1, 180, 18 | spring parameters |
| `min`, `max` | ±∞ | clamp |

## Easing

`linear`, `in_out_cubic`, `out_cubic`, `out_quint`, `out_expo`, `out_back` (overshoots), `bezier` (CSS-style cubic), `spring` (physical, keeps velocity when retargeted).

`parse_easing` accepts the same names as strings.

## Frame semantics

`vw::App` calls `begin_animation_frame()` before each build and reads `animations_active()` after it: a build in which any tween is still moving (or that called `request_animation_continuation()`) asks for another frame. Tweens live for the program's lifetime and are keyed only by id, so ids should be stable for one logical value and fresh for one-shot effects.
