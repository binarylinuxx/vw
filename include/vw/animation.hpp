#pragma once

#include <array>
#include <limits>
#include <optional>
#include <string_view>

namespace vw {

enum class Easing { linear, in_out_cubic, out_cubic, out_quint, out_expo, out_back, bezier, spring };

struct AnimationOptions {
  double duration = 0.28;
  // Starting value the first time this id is seen; defaults to the target.
  std::optional<double> initial;
  Easing easing = Easing::out_cubic;
  // Cubic Bezier control points (x1, y1, x2, y2) for Easing::bezier.
  std::array<double, 4> bezier{0.25, 0.1, 0.25, 1};
  double mass = 1;
  double stiffness = 180;
  double damping = 18;
  double min = -std::numeric_limits<double>::infinity();
  double max = std::numeric_limits<double>::infinity();
};

// Immediate-mode tween keyed by id: returns the current value while moving
// towards target, and keeps frames coming until every tween has settled.
double animate(std::string_view id, double target, const AnimationOptions& options = {});
std::optional<Easing> parse_easing(std::string_view name);

void begin_animation_frame();
bool animations_active();
// Keeps rendering frames for one more refresh.
void request_animation_continuation();

}  // namespace vw
