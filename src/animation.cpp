#include "vw/animation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
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
  // Value per second carried in from the tween this one replaced. Springs
  // start with it; eased tweens add a Hermite term that keeps the motion
  // continuous and fades out by the end.
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
std::optional<Clock::time_point> next_frame_time;
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
double eased(const Tween& tween, double t) {
  return tween.easing == Easing::bezier ? bezier_value(t, tween.bezier) : ease(t, tween.easing);
}

// The easing curve's slope, numerically.
double slope(const Tween& tween, double t) {
  constexpr double step = 1e-4;
  const double low = std::max(0.0, t - step), high = std::min(1.0, t + step);
  return (eased(tween, high) - eased(tween, low)) / (high - low);
}

struct Sample { double value; double velocity; bool running; };
Sample sample(const Tween& tween, Clock::time_point now) {
  const double elapsed = std::max(0.0, std::chrono::duration<double>(now - tween.started).count());
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
  const double t = std::clamp(elapsed / tween.duration, 0.0, 1.0);
  const double delta = tween.target - tween.from;
  // Carried velocity, minus what the curve itself starts with, enters through
  // h(t) = t(1-t)^2: zero at both ends, slope 1 at the start, flat at the end.
  const double carried = tween.velocity == 0 ? 0 : tween.velocity - delta * slope(tween, 0) / tween.duration;
  const double hermite = t * (1 - t) * (1 - t);
  const double result = tween.from + delta * eased(tween, t) + carried * tween.duration * hermite;
  const double velocity = delta * slope(tween, t) / tween.duration + carried * (3 * t * t - 4 * t + 1);
  const bool running = (tween.from != tween.target || carried != 0) && t < 1;
  if (!running) return {std::clamp(tween.target, tween.minimum, tween.maximum), 0, false};
  const double bounded = std::clamp(result, tween.minimum, tween.maximum);
  return {bounded, bounded == result ? velocity : 0, true};
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
  frame_now = next_frame_time.value_or(Clock::now());
  next_frame_time.reset();
}
void set_next_frame_time(Clock::time_point time) { next_frame_time = time; }
bool animations_active() {
  // Ends the build: tweens started from handlers use the real time again.
  frame_now.reset();
  return active;
}
void request_animation_continuation() { active = true; }
}  // namespace vw
