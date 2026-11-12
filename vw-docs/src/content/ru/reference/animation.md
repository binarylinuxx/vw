---
title: Анимация
description: API vw/animation.hpp целиком. Руководство по движению и жестам показывает его в деле.
---

```cpp
double vw::animate(std::string_view id, double target, const vw::AnimationOptions& options = {});
std::optional<vw::Easing> vw::parse_easing(std::string_view name);
void vw::request_animation_continuation();
bool vw::animations_active();
void vw::begin_animation_frame();
```

## AnimationOptions

| Поле | По умолчанию | Значение |
| --- | --- | --- |
| `duration` | 0.28 | секунды; `0` — сразу к цели |
| `initial` | цель | начальное значение, когда `id` встречается впервые |
| `easing` | `out_cubic` | см. ниже |
| `bezier` | `{0.25, 0.1, 0.25, 1}` | контрольные точки для `Easing::bezier` |
| `mass`, `stiffness`, `damping` | 1, 180, 18 | параметры пружины |
| `min`, `max` | ±∞ | ограничение |

## Easing

`linear`, `in_out_cubic`, `out_cubic`, `out_quint`, `out_expo`, `out_back` (с перелётом), `bezier` (кубическая кривая как в CSS), `spring` (физическая, сохраняет скорость при смене цели).

`parse_easing` принимает те же имена строками.

## Семантика кадров

`vw::App` вызывает `begin_animation_frame()` перед каждой сборкой и читает `animations_active()` после неё: сборка, в которой хоть одна анимация ещё движется (или которая вызвала `request_animation_continuation()`), просит ещё один кадр. Анимации живут всё время работы программы и различаются только по id, так что id должны быть стабильными для одного логического значения и новыми для разовых эффектов.
