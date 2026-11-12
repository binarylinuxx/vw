#pragma once

// What facetc's generated C++ leans on. Small on purpose: everything else is
// plain vw.

#include <vw/vw.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <functional>
#include <iterator>
#include <span>
#include <unordered_map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace facet {

template <class T>
concept text_like = std::is_convertible_v<const T&, std::string_view>;

template <class T>
std::string to_string(const T& value) {
  if constexpr (text_like<T>) return std::string(std::string_view(value));
  else return std::format("{}", value);
}

// Facet's +: joins text when either side is text, adds numbers otherwise.
template <class A, class B>
auto add(const A& left, const B& right) {
  if constexpr (text_like<A> || text_like<B>) return to_string(left) + to_string(right);
  else return left + right;
}

// vw layouts need a child, and vw rejects empty geometry: an empty for or
// if leaves a 1x1 invisible area behind.
inline vw::Widget nothing(vw::Ui& ui) {
  vw::HitAreaProps area;
  area.width = area.height = 1;
  return ui.hit_area(area);
}

// An image whose path and icon are empty (an app without an icon) keeps its
// place in the layout and draws nothing.
inline vw::Widget image(vw::Ui& ui, const vw::ImageProps& props) {
  if (!props.path.empty() || !props.icon.empty() || !props.bitmap.empty()) return ui.image(props);
  vw::HitAreaProps area;
  area.width = std::max(1.0F, props.width);
  area.height = std::max(1.0F, props.height);
  return ui.hit_area(area);
}

// padding: on a Text or Image: the same margin on every side.
inline vw::Margin margin(double padding) {
  const auto value = static_cast<std::int32_t>(std::lround(padding));
  return {value, value, value, value};
}

// A ListView whose rows come from `render(ui, index)`. Without item_height
// the first row is measured; without width that row's width is used; without
// height the list is as tall as all its rows.
template <class Render>
vw::Widget listview(vw::Ui& ui, vw::ListviewProps props, const Render& render) {
  if (props.count == 0) return nothing(ui);
  if (props.item_height <= 0.0F || props.width <= 0.0F) {
    vw::Scene probe;
    vw::Ui measure(probe);
    const vw::Widget first = render(measure, 0);
    if (props.item_height <= 0.0F) props.item_height = std::max(1.0F, first.height);
    if (props.width <= 0.0F) props.width = std::max(1.0F, first.width);
  }
  if (props.height <= 0.0F)
    props.height = props.item_height * static_cast<float>(props.count) +
                   props.gap * static_cast<float>(props.count - 1);
  props.render = [&](std::size_t index) -> std::optional<vw::Widget> { return render(ui, index); };
  return ui.listview(props);
}

// Props spelled out field by field: clang warns on designated initializers
// that leave fields out.
inline vw::StackProps sized(float width, float height) {
  vw::StackProps props;
  props.width = width;
  props.height = height;
  return props;
}

// A tween for animate(): `milliseconds` long, keyed by `id`.
inline float tween(const std::string& id, double target, double milliseconds,
                   vw::Easing easing = vw::Easing::out_cubic) {
  vw::AnimationOptions options;
  options.duration = milliseconds / 1000.0;
  options.easing = easing;
  return static_cast<float>(vw::animate(id, target, options));
}

// A rectangle with content: the content centred on top of it. Without
// content (an if that drew nothing) it is just the rectangle.
inline vw::Widget on(vw::Ui& ui, const vw::Widget& base, const std::vector<vw::Widget>& children) {
  if (children.empty()) return base;
  const vw::Widget content = children.size() == 1 ? children.front() : ui.column(children, {});
  return ui.stack({base, content}, sized(base.width, base.height));
}

inline vw::Widget row(vw::Ui& ui, const std::vector<vw::Widget>& children, const vw::LayoutProps& props) {
  return children.empty() ? nothing(ui) : ui.row(children, props);
}

inline vw::Widget column(vw::Ui& ui, const std::vector<vw::Widget>& children, const vw::LayoutProps& props) {
  return children.empty() ? nothing(ui) : ui.column(children, props);
}

inline vw::Widget grid(vw::Ui& ui, const std::vector<vw::Widget>& children, const vw::GridProps& props) {
  return children.empty() ? nothing(ui) : ui.grid(children, props);
}

// Several children where one is expected stack into a column.
inline vw::Widget group(vw::Ui& ui, const std::vector<vw::Widget>& children) {
  return children.size() == 1 ? children.front() : column(ui, children, {});
}

// A key for state that must follow one element: its place in the .facet
// file plus the keys of the loops around it, e.g. "panel.facet:12:5|firefox".
template <class... Keys>
std::string key(std::string_view place, const Keys&... keys) {
  std::string result(place);
  ((result += '|', result += to_string(keys)), ...);
  return result;
}

// Text items for a Dropdown from any range of text-like values.
template <class Range>
std::vector<std::string> strings(const Range& range) {
  std::vector<std::string> result;
  for (const auto& item : range) result.push_back(to_string(item));
  return result;
}

// A layout's background: drawn first (so it sits behind the children), then
// sized to the laid-out content once that is known.
inline vw::Widget behind(vw::Ui& ui, vw::Widget back, const vw::Widget& content) {
  auto& rectangle = ui.scene().rectangles[back.begin];
  rectangle.width = content.width;
  rectangle.height = content.height;
  back.width = content.width;
  back.height = content.height;
  return ui.stack({back, content}, sized(content.width, content.height));
}

// Room left along a row (or column) for its flex children, which sit in
// `slots` and are not laid out yet.
inline float flex_space(const std::vector<vw::Widget>& children, std::span<const std::size_t> slots,
                        float main, float padding, float gap, bool row) {
  float used = padding * 2.0F + gap * static_cast<float>(children.empty() ? 0 : children.size() - 1);
  for (std::size_t index = 0; index < children.size(); ++index) {
    if (std::ranges::find(slots, index) != slots.end()) continue;
    const vw::Widget& child = children[index];
    used += row ? child.width + child.margin[3] + child.margin[1]
                : child.height + child.margin[0] + child.margin[2];
  }
  return std::max(0.0F, main - used);
}

inline vw::Color mix(const vw::Color& from, const vw::Color& to, double amount) {
  const auto t = static_cast<float>(std::clamp(amount, 0.0, 1.0));
  return {from[0] + (to[0] - from[0]) * t, from[1] + (to[1] - from[1]) * t,
          from[2] + (to[2] - from[2]) * t, from[3] + (to[3] - from[3]) * t};
}

// --- controls: ui.Toggle, ui.Slider, ui.Dropdown, ui.Spinner ---------------

struct ToggleProps {
  std::string id;
  bool value = false;
  float width = 44.0F;
  float height = 24.0F;
  vw::Color color = vw::rgb(0xa7cbe7);
  vw::Color off_color = vw::rgb(0x20272c);
  vw::Color thumb_color = vw::rgb(0xdfe6ee);
  std::function<void(bool)> on_change;
};

inline vw::Widget toggle(vw::Ui& ui, const ToggleProps& props) {
  const double on = tween(props.id, props.value ? 1 : 0, 180);
  constexpr float inset = 3.0F;
  const float knob = props.height - inset * 2.0F;
  vw::RectangleProps track{};
  track.width = props.width;
  track.height = props.height;
  track.radius = props.height * 0.5F;
  track.color = mix(props.off_color, props.color, on);
  track.input.on_click = [on_change = props.on_change, value = props.value](const vw::ClickEvent&) {
    if (on_change) on_change(!value);
  };
  const vw::Widget body = ui.rectangle(track);
  vw::RectangleProps thumb{};
  thumb.width = thumb.height = knob;
  thumb.radius = knob * 0.5F;
  thumb.color = props.thumb_color;
  thumb.place.anchor = vw::anchor::left;
  thumb.place.margin = {0, 0, 0, static_cast<std::int32_t>(std::lround(inset + on * (props.width - knob - inset * 2.0F)))};
  const vw::Widget knob_widget = ui.rectangle(thumb);
  return ui.stack({body, knob_widget}, sized(props.width, props.height));
}

struct SliderProps {
  std::string id;
  float value = 0.0F;
  float min = 0.0F;
  float max = 100.0F;
  float step = 0.0F;  // 0: continuous
  float width = 200.0F;
  float height = 24.0F;
  vw::Color color = vw::rgb(0xa7cbe7);
  vw::Color track_color = vw::rgb(0x20272c);
  vw::Color thumb_color = vw::rgb(0xdfe6ee);
  std::function<void(float)> on_change;
};

// Where each slider's drag started, by id.
inline std::unordered_map<std::string, float>& slider_drags() {
  static std::unordered_map<std::string, float> drags;
  return drags;
}

inline vw::Widget slider(vw::Ui& ui, const SliderProps& props) {
  const float range = props.max > props.min ? props.max - props.min : 1.0F;
  const float fraction = std::clamp((props.value - props.min) / range, 0.0F, 1.0F);
  constexpr float bar = 6.0F;
  const float knob = std::min(props.height, 18.0F);
  vw::HitAreaProps area{};
  area.width = props.width;
  area.height = props.height;
  area.input.on_drag_start = [id = props.id, value = props.value](const vw::DragEvent&) { slider_drags()[id] = value; };
  area.input.on_drag = [props, range, knob](const vw::DragEvent& drag) {
    const auto start = slider_drags().find(props.id);
    if (start == slider_drags().end() || !props.on_change) return;
    float value = start->second + static_cast<float>(drag.dx) / std::max(1.0F, props.width - knob) * range;
    if (props.step > 0.0F) value = props.min + std::round((value - props.min) / props.step) * props.step;
    props.on_change(std::clamp(value, props.min, props.max));
  };
  area.input.on_drag_end = [id = props.id](const vw::DragEvent&) { slider_drags().erase(id); };
  const vw::Widget hit = ui.hit_area(area);
  const auto bar_props = [&](float width, const vw::Color& color) {
    vw::RectangleProps rectangle{};
    rectangle.width = std::max(width, bar);
    rectangle.height = bar;
    rectangle.radius = bar * 0.5F;
    rectangle.color = color;
    rectangle.place.anchor = vw::anchor::left;
    return rectangle;
  };
  const vw::Widget track = ui.rectangle(bar_props(props.width, props.track_color));
  const vw::Widget fill = ui.rectangle(bar_props(knob * 0.5F + fraction * (props.width - knob), props.color));
  vw::RectangleProps thumb{};
  thumb.width = thumb.height = knob;
  thumb.radius = knob * 0.5F;
  thumb.color = props.thumb_color;
  thumb.place.anchor = vw::anchor::left;
  thumb.place.margin = {0, 0, 0, static_cast<std::int32_t>(std::lround(fraction * (props.width - knob)))};
  const vw::Widget knob_widget = ui.rectangle(thumb);
  return ui.stack({hit, track, fill, knob_widget}, sized(props.width, props.height));
}

struct DropdownProps {
  std::string id;
  std::vector<std::string> options;
  std::string current;
  float width = 200.0F;
  float height = 36.0F;
  vw::Color color = vw::rgb(0x20272c);
  vw::Color hover_color = vw::rgb(0x262d33);
  vw::Color text_color = vw::rgb(0xdfe6ee);
  std::function<void(const std::string&)> on_change;
};

// Which dropdowns are open, by id.
inline std::unordered_map<std::string, bool>& dropdowns_open() {
  static std::unordered_map<std::string, bool> open;
  return open;
}

inline vw::Widget dropdown(vw::Ui& ui, const DropdownProps& props) {
  const bool open = dropdowns_open()[props.id];
  const auto row = [&](const std::string& label, std::function<void()> pressed, bool header) {
    vw::RectangleProps box{};
    box.width = props.width;
    box.height = props.height;
    box.radius = 10.0F;
    box.color = props.color;
    box.input.on_click = [pressed = std::move(pressed)](const vw::ClickEvent&) { pressed(); };
    box.input.hover = vw::Hover{};
    box.input.hover->color = props.hover_color;
    const vw::Widget body = ui.rectangle(box);
    vw::TextProps text{};
    text.text = header ? label + (open ? "  ▴" : "  ▾") : label;
    text.size = 14.0F;
    text.color = props.text_color;
    text.max_width = props.width - 24.0F;
    text.place.anchor = vw::anchor::left;
    text.place.margin = {0, 0, 0, 12};
    const vw::Widget label_widget = ui.text(text);
    return ui.stack({body, label_widget}, sized(props.width, props.height));
  };
  std::vector<vw::Widget> rows;
  rows.push_back(row(props.current, [id = props.id] { dropdowns_open()[id] = !dropdowns_open()[id]; }, true));
  if (open) {
    for (const auto& option : props.options)
      rows.push_back(row(option, [id = props.id, option, on_change = props.on_change] {
        dropdowns_open()[id] = false;
        if (on_change) on_change(option);
      }, false));
  }
  vw::LayoutProps layout;
  layout.gap = 4.0F;
  return ui.column(rows, layout);
}

struct SpinnerProps {
  std::string id;
  float size = 24.0F;
  vw::Color color = vw::rgb(0xa7cbe7);
};

// An arc turning once a second, drawn by a canvas.
inline vw::Widget spinner(vw::Ui& ui, const SpinnerProps& props) {
  const auto channel = [&](int index) { return static_cast<int>(std::lround(props.color[index] * 255.0F)); };
  const std::string code = std::format(
      "function draw(ctx, canvas, time) {{\n"
      "  const r = Math.min(canvas.width, canvas.height) / 2 - 3;\n"
      "  const a = (time % 1) * Math.PI * 2;\n"
      "  ctx.clearRect(0, 0, canvas.width, canvas.height);\n"
      "  ctx.lineWidth = 3; ctx.lineCap = 'round';\n"
      "  ctx.strokeStyle = 'rgba({}, {}, {}, {})';\n"
      "  ctx.beginPath(); ctx.arc(canvas.width / 2, canvas.height / 2, r, a, a + 4.2); ctx.stroke();\n"
      "}}\n",
      channel(0), channel(1), channel(2), props.color[3]);
  vw::CanvasProps canvas;
  canvas.id = props.id;
  canvas.code = code;
  canvas.width = canvas.height = props.size;
  canvas.animate = true;
  return ui.canvas(canvas);
}

}  // namespace facet
