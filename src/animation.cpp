#include "vw/animation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace vw {
namespace {
using Clock = std::chrono::steady_clock;
struct Tween {
  double from = 0;
  double target = 0;
  double duration = 0.28;
  Easing easing = Easing::out_cubic;
  Clock::time_point started;
  std::array<double, 4> bezier{0.25, 0.1, 0.25, 1};
  double mass = 1;
  double stiffness = 180;
  double damping = 18;
  double velocity = 0;
  double minimum = -std::numeric_limits<double>::infinity();
  double maximum = std::numeric_limits<double>::infinity();
};
struct NameHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view name) const { return std::hash<std::string_view>{}(name); }
};
std::unordered_map<std::string, Tween, NameHash, std::equal_to<>> tweens;
bool active = false;
// One instant per build, so every tween in a frame samples the same time and
// moves in lockstep, however long the build itself takes.
std::optional<Clock::time_point> frame_now;
double ease(double t, Easing mode) {
  switch (mode) {
    case Easing::linear: return t;
    case Easing::in_out_cubic: return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
    case Easing::out_cubic: return 1 - std::pow(1 - t, 3);
    case Easing::out_quint: return 1 - std::pow(1 - t, 5);
    case Easing::out_expo: return t >= 1 ? 1 : 1 - std::pow(2, -10 * t);
    case Easing::out_back: return 1 + 2.70158 * std::pow(t - 1, 3) + 1.70158 * std::pow(t - 1, 2);
    case Easing::bezier:
    case Easing::spring: break;
  }
  return t;
}
double bezier_value(double t, const std::array<double, 4>& control) {
  const auto curve = [](double u, double a, double b) {
    return 3 * (1 - u) * (1 - u) * u * a + 3 * (1 - u) * u * u * b + u * u * u;
  };
  double lo = 0, hi = 1;
  for (int iteration = 0; iteration < 28; ++iteration) {
    const double mid = (lo + hi) / 2;
    if (curve(mid, control[0], control[2]) < t) lo = mid;
    else hi = mid;
  }
  return curve((lo + hi) / 2, control[1], control[3]);
}
struct Sample { double value; double velocity; bool running; };
Sample sample(const Tween& tween, Clock::time_point now) {
  const double elapsed = std::chrono::duration<double>(now - tween.started).count();
  if (tween.duration <= 0) return {tween.target, 0, false};
  if (tween.easing == Easing::spring) {
    const double omega = std::sqrt(tween.stiffness / tween.mass);
    const double decay = tween.damping / (2 * tween.mass);
    const double initial = tween.from - tween.target;
    double displacement, velocity;
    if (std::abs(decay - omega) < 0.0001) {
      const double b = tween.velocity + omega * initial;
      const double e = std::exp(-omega * elapsed);
      displacement = e * (initial + b * elapsed);
      velocity = e * (b - omega * (initial + b * elapsed));
    } else if (decay < omega) {
      const double frequency = std::sqrt(omega * omega - decay * decay);
      const double b = (tween.velocity + decay * initial) / frequency;
      const double cosine = std::cos(frequency * elapsed), sine = std::sin(frequency * elapsed);
      const double e = std::exp(-decay * elapsed);
      displacement = e * (initial * cosine + b * sine);
      velocity = e * ((-decay * initial + frequency * b) * cosine +
                      (-decay * b - frequency * initial) * sine);
    } else {
      const double root = std::sqrt(decay * decay - omega * omega);
      const double r1 = -decay + root, r2 = -decay - root;
      const double a = (tween.velocity - r2 * initial) / (r1 - r2);
      const double b = initial - a;
      displacement = a * std::exp(r1 * elapsed) + b * std::exp(r2 * elapsed);
      velocity = a * r1 * std::exp(r1 * elapsed) + b * r2 * std::exp(r2 * elapsed);
    }
    const double tolerance = std::max(0.001, std::abs(initial) * 0.001);
    const bool running = elapsed < 10 && (std::abs(displacement) > tolerance ||
                                         std::abs(velocity) > tolerance * omega);
    if (!running) return {tween.target, 0, false};
    const double raw = tween.target + displacement;
    const double bounded = std::clamp(raw, tween.minimum, tween.maximum);
    return {bounded, raw == bounded ? velocity : 0, true};
  }
  const double t = tween.duration <= 0 ? 1 : std::clamp(
      elapsed / tween.duration, 0.0, 1.0);
  const double progress = tween.easing == Easing::bezier ? bezier_value(t, tween.bezier) : ease(t, tween.easing);
  const double result = tween.from + (tween.target - tween.from) * progress;
  const bool running = tween.from != tween.target && t < 1;
  return {std::clamp(running ? result : tween.target, tween.minimum, tween.maximum), 0, running};
}
}  // namespace

std::optional<Easing> parse_easing(std::string_view mode) {
  if (mode == "linear") return Easing::linear;
  if (mode == "in_out_cubic") return Easing::in_out_cubic;
  if (mode == "out_cubic") return Easing::out_cubic;
  if (mode == "out_quint") return Easing::out_quint;
  if (mode == "out_expo") return Easing::out_expo;
  if (mode == "out_back") return Easing::out_back;
  if (mode == "bezier") return Easing::bezier;
  if (mode == "spring") return Easing::spring;
  return std::nullopt;
}

double animate(std::string_view id, double target, const AnimationOptions& options) {
  if (id.empty() || !std::isfinite(target))
    throw std::invalid_argument("animation needs an id and finite target");
  double initial = options.initial.value_or(target);
  if (!std::isfinite(options.duration) || options.duration < 0 || !std::isfinite(initial))
    throw std::invalid_argument("animation duration must be finite and non-negative");
  for (double control : options.bezier)
    if (!std::isfinite(control)) throw std::invalid_argument("Bezier controls must be finite");
  if (options.bezier[0] < 0 || options.bezier[0] > 1 || options.bezier[2] < 0 || options.bezier[2] > 1)
    throw std::invalid_argument("Bezier x control points must be in [0,1]");
  if (!std::isfinite(options.mass) || !std::isfinite(options.stiffness) ||
      !std::isfinite(options.damping) || options.mass < 0.0001 || options.stiffness < 0.0001 ||
      options.damping <= 0 || std::isnan(options.min) || std::isnan(options.max) ||
      options.min > options.max)
    throw std::invalid_argument("invalid spring parameters or animation bounds");
  target = std::clamp(target, options.min, options.max);
  initial = std::clamp(initial, options.min, options.max);
  if (!std::isfinite(target) || !std::isfinite(initial))
    throw std::invalid_argument("animation bounds must permit finite values");
  const auto now = frame_now.value_or(Clock::now());
  Tween next;
  next.from = initial;
  next.target = target;
  next.duration = options.duration;
  next.easing = options.easing;
  next.started = now;
  next.bezier = options.bezier;
  next.mass = options.mass;
  next.stiffness = options.stiffness;
  next.damping = options.damping;
  next.minimum = options.min;
  next.maximum = options.max;
  auto found = tweens.find(id);
  if (found == tweens.end()) {
    found = tweens.emplace(std::string(id), next).first;
  } else if (found->second.target != target || options.duration == 0) {
    const Sample current = sample(found->second, now);
    next.from = current.value;
    next.velocity = current.velocity;
    found->second = next;
  }
  auto& tween = found->second;
  tween.minimum = options.min;
  tween.maximum = options.max;
  const Sample current = sample(tween, now);
  active = active || current.running;
  return current.value;
}

void begin_animation_frame() {
  active = false;
  frame_now = Clock::now();
}
bool animations_active() {
  // Ends the build: tweens started from handlers use the real time again.
  frame_now.reset();
  return active;
}
void request_animation_continuation() { active = true; }
}  // namespace vw
