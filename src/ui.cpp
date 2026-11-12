#include "vw/ui.hpp"
#include "vw/canvas.hpp"
#include "vw/host.hpp"
#include "vw/icon.hpp"
#include "vw/image.hpp"
#include "vw/text.hpp"

#include <algorithm>
#include <limits>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace vw {
namespace {

Color clamped(Color color) {
  for (float& channel : color) channel = std::clamp(channel, 0.0F, 1.0F);
  return color;
}

std::uint32_t resolve_anchor(const Placement& place, bool position_implies_top_left) {
  if (place.anchor) return *place.anchor;
  return position_implies_top_left && (place.x || place.y) ? anchor::top_left : anchor::center;
}

void apply_clip(const Placement& place, Rectangle& style) {
  if (!place.clip) return;
  const Clip& clip = *place.clip;
  if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.width) ||
      !std::isfinite(clip.height) || clip.width <= 0.0F || clip.height <= 0.0F)
    throw std::invalid_argument("clip requires finite coordinates and positive dimensions");
  style.clip = {clip.x, clip.y, clip.width, clip.height};
  style.clip_radius = clip.radius;
}

Rectangle base_style(const Placement& place, bool position_implies_top_left) {
  Rectangle style;
  style.x = place.x.value_or(0.0F);
  style.y = place.y.value_or(0.0F);
  style.anchor = resolve_anchor(place, position_implies_top_left);
  style.layer = static_cast<std::uint32_t>(place.layer);
  style.margin = place.margin;
  style.exclusive = place.exclusive || place.exclusive_zone != 0;
  style.exclusive_zone = place.exclusive_zone;
  style.visible = place.visible;
  apply_clip(place, style);
  return style;
}

bool contains(const Widget& widget, std::size_t begin, std::size_t end) {
  return begin >= widget.begin && end <= widget.end;
}

void shift(Scene& scene, const Widget& widget, float x, float y) {
  for (std::size_t index = widget.begin; index < widget.end; ++index) {
    auto& rectangle = scene.rectangles[index];
    rectangle.x += x;
    rectangle.y += y;
    if (rectangle.clip[2] > 0.0F && rectangle.clip[3] > 0.0F) {
      rectangle.clip[0] += x;
      rectangle.clip[1] += y;
    }
  }
  for (Interaction& interaction : scene.interactions) {
    if (!contains(widget, interaction.begin, interaction.end)) continue;
    interaction.x += x;
    interaction.y += y;
    if (interaction.clip[2] > 0.0F && interaction.clip[3] > 0.0F) {
      interaction.clip[0] += x;
      interaction.clip[1] += y;
    }
  }
  for (ScrollArea& area : scene.scroll_areas) {
    if (!contains(widget, area.begin, area.end)) continue;
    area.x += x;
    area.y += y;
  }
}

void hide_range(Scene& scene, const Widget& widget) {
  for (std::size_t index = widget.begin; index < widget.end; ++index) {
    scene.rectangles[index].visible = false;
    scene.rectangles[index].raster_visible = false;
  }
  for (Interaction& interaction : scene.interactions)
    if (contains(widget, interaction.begin, interaction.end)) interaction.enabled = false;
  for (ScrollArea& area : scene.scroll_areas)
    if (contains(widget, area.begin, area.end)) area.enabled = false;
}

std::array<float, 4> intersect_clip(const std::array<float, 4>& previous,
                                     const std::array<float, 4>& viewport) {
  if (previous[2] <= 0.0F || previous[3] <= 0.0F) return viewport;
  const float left = std::max(previous[0], viewport[0]);
  const float top = std::max(previous[1], viewport[1]);
  return {left, top,
          std::max(0.0F, std::min(previous[0] + previous[2], viewport[0] + viewport[2]) - left),
          std::max(0.0F, std::min(previous[1] + previous[3], viewport[1] + viewport[3]) - top)};
}

// Clips every primitive, interaction and scroll area in [begin, end) to the
// viewport, hiding anything that ends up fully outside it.
void clip_range(Scene& scene, std::size_t begin, std::size_t end,
                const std::array<float, 4>& viewport,
                std::optional<float> radius = std::nullopt) {
  for (std::size_t index = begin; index < end; ++index) {
    auto& rectangle = scene.rectangles[index];
    rectangle.clip = intersect_clip(rectangle.clip, viewport);
    if (radius) rectangle.clip_radius = *radius;
    const auto& clip = rectangle.clip;
    if (clip[2] <= 0.0F || clip[3] <= 0.0F ||
        rectangle.x >= clip[0] + clip[2] || rectangle.y >= clip[1] + clip[3] ||
        rectangle.x + rectangle.width <= clip[0] || rectangle.y + rectangle.height <= clip[1]) {
      rectangle.visible = false;
      rectangle.raster_visible = false;
    }
  }
  for (Interaction& interaction : scene.interactions) {
    if (interaction.begin >= begin && interaction.end <= end) {
      interaction.clip = intersect_clip(interaction.clip, viewport);
      interaction.enabled = interaction.enabled &&
                            interaction.clip[2] > 0.0F && interaction.clip[3] > 0.0F;
    }
  }
  for (ScrollArea& area : scene.scroll_areas) {
    if (area.begin >= begin && area.end <= end) {
      const auto clipped = intersect_clip({area.x, area.y, area.width, area.height}, viewport);
      area.x = clipped[0];
      area.y = clipped[1];
      area.width = clipped[2];
      area.height = clipped[3];
      area.enabled = area.enabled && area.width > 0.0F && area.height > 0.0F;
    }
  }
}

std::size_t first_begin(std::span<const Widget> children) {
  return std::ranges::min_element(children, {}, &Widget::begin)->begin;
}

void grow_scene(Scene& scene, float right, float bottom) {
  scene.width = std::max(scene.width, static_cast<std::uint32_t>(std::ceil(right)));
  scene.height = std::max(scene.height, static_cast<std::uint32_t>(std::ceil(bottom)));
}

std::unordered_map<std::string, std::shared_ptr<TextFieldState>>& text_field_states() {
  static std::unordered_map<std::string, std::shared_ptr<TextFieldState>> states;
  return states;
}

std::shared_ptr<ScrollState> scroll_state(const std::string& id) {
  static std::unordered_map<std::string, std::shared_ptr<ScrollState>> states;
  if (id.empty()) throw std::invalid_argument("scroll id cannot be empty");
  auto& state = states[id];
  if (!state) state = std::make_shared<ScrollState>();
  return state;
}

Rectangle scroll_style(const Placement& place, float width, float height, float radius) {
  Rectangle style = base_style(place, false);
  style.clip = {0.0F, 0.0F, 0.0F, 0.0F};
  style.width = width;
  style.height = height;
  style.radius = radius;
  return style;
}

std::string canvas_source(const CanvasProps& props) {
  if (!props.code.empty()) return props.code;
  struct Script { std::filesystem::file_time_type modified; std::string code; };
  static std::unordered_map<std::string, Script> scripts;
  const auto modified = std::filesystem::last_write_time(props.script);
  auto found = scripts.find(props.script);
  if (found == scripts.end() || found->second.modified != modified) {
    std::ifstream file(props.script);
    if (!file) throw std::runtime_error("could not open canvas script: " + props.script);
    std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    found = scripts.insert_or_assign(props.script, Script{modified, std::move(source)}).first;
  }
  return found->second.code;
}

std::uint32_t surface_dimension(float value, const char* what) {
  if (!std::isfinite(value) || value < 1.0F || value > 8192.0F)
    throw std::invalid_argument(std::string(what) + " dimensions must be between 1 and 8192");
  return static_cast<std::uint32_t>(std::ceil(value));
}

}  // namespace

Widget Ui::push(std::size_t begin, float width, float height, const Rectangle& style,
                const Input& input) {
  scene_.anchor = style.anchor;
  scene_.layer = style.layer;
  scene_.margin = style.margin;
  scene_.exclusive = style.exclusive;
  scene_.exclusive_zone = style.exclusive_zone;
  Interaction interaction;
  interaction.begin = begin;
  interaction.end = scene_.rectangles.size();
  interaction.x = style.x;
  interaction.y = style.y;
  interaction.width = width;
  interaction.height = height;
  interaction.clip = style.clip;
  interaction.on_click = input.on_click;
  interaction.on_drag_start = input.on_drag_start;
  interaction.on_drag = input.on_drag;
  interaction.on_drag_end = input.on_drag_end;
  interaction.on_scroll = input.on_scroll;
  interaction.on_hover = input.on_hover;
  interaction.drag_button = input.drag_button;
  if (input.hover) {
    interaction.hover_scale = input.hover->scale;
    interaction.duration = input.hover->duration;
    interaction.has_hover_color = input.hover->color.has_value();
    if (input.hover->color) interaction.hover_color = clamped(*input.hover->color);
  }
  if (style.visible && (interaction.on_click || interaction.has_hover_color ||
                        interaction.hover_scale != 1.0F || interaction.draggable() ||
                        interaction.on_scroll || interaction.on_hover)) {
    for (std::size_t index = begin; index < scene_.rectangles.size(); ++index)
      interaction.original_colors.push_back(scene_.rectangles[index].color);
    scene_.interactions.push_back(std::move(interaction));
  }
  return Widget{begin, scene_.rectangles.size(), width, height, style.margin, style.anchor};
}

Widget Ui::rectangle(const RectangleProps& props) {
  Rectangle rectangle = base_style(props.place, true);
  rectangle.width = props.width;
  rectangle.height = props.height;
  rectangle.radius = props.radius;
  for (std::size_t index = 0; index < 4; ++index) {
    if (!props.corner_radii[index]) continue;
    const float radius = *props.corner_radii[index];
    if (!std::isfinite(radius) || radius < 0.0F)
      throw std::invalid_argument("corner radius must be finite and non-negative");
    rectangle.corner_radii[index] = radius;
  }
  rectangle.border_width = props.border_width;
  rectangle.position_configured = props.place.x || props.place.y || props.place.anchor;
  rectangle.color = clamped(props.color);
  rectangle.color2 = clamped(props.color2.value_or(props.color));
  rectangle.border_color = clamped(props.border_color);
  rectangle.gradient = static_cast<std::uint32_t>(props.gradient);
  rectangle.shader = props.shader;
  for (float parameter : props.shader_params)
    if (!std::isfinite(parameter)) throw std::invalid_argument("shader parameters must be finite");
  rectangle.shader_params = props.shader_params;
  for (float value : props.shader_data)
    if (!std::isfinite(value)) throw std::invalid_argument("shader data must be finite");
  rectangle.shader_data = props.shader_data;
  const std::size_t begin = scene_.rectangles.size();
  scene_.add(rectangle);
  return push(begin, rectangle.width, rectangle.height, rectangle, props.input);
}

Widget Ui::hit_area(const HitAreaProps& props) {
  Rectangle style = base_style(props.place, false);
  style.width = props.width;
  style.height = props.height;
  const std::size_t begin = scene_.rectangles.size();
  scene_.add(style);
  scene_.rectangles.back().visible = false;
  scene_.rectangles.back().raster_visible = false;
  return push(begin, style.width, style.height, style, props.input);
}

Widget Ui::text(const TextProps& props) {
  Rectangle style = base_style(props.place, true);
  style.radius = props.radius;
  style.color = clamped(props.color);
  style.color2 = style.color;
  style.raster_visible = style.visible;
  const std::size_t begin = scene_.rectangles.size();
  const TextOptions options{props.max_width, props.max_lines, props.align, props.ellipsize};
  const auto [width, height] =
      add_text(scene_, props.text, props.font, props.size, props.weight, style, options);
  return push(begin, width, height, style, props.input);
}

Widget Ui::image(const ImageProps& props) {
  if (props.path.empty() && props.icon.empty())
    throw std::invalid_argument("image requires path or icon");
  Rectangle style = base_style(props.place, true);
  style.radius = props.radius;
  style.color = clamped(props.tint);
  style.raster_visible = style.visible;
  const auto source = props.path.empty()
      ? resolve_icon(props.icon, static_cast<int>(std::max({48.0F, props.width, props.height})))
      : std::filesystem::path(props.path);
  if (source.empty()) throw std::runtime_error("icon was not found: " + props.icon);
  const std::size_t begin = scene_.rectangles.size();
  const auto [width, height] =
      add_image(scene_, source, props.width, props.height, style, props.fit);
  scene_.rasters.back().effect = props.effect;
  if (!props.mask.empty()) {
    if (props.mask.size() < 6 || props.mask.size() % 2 != 0)
      throw std::invalid_argument("image mask needs at least three x, y points");
    scene_.rasters.back().mask = props.mask;
  }
  return push(begin, width, height, style, props.input);
}

Widget Ui::canvas(const CanvasProps& props) {
  if (props.id.empty()) throw std::invalid_argument("canvas requires an id");
  if (props.code.empty() && props.script.empty())
    throw std::invalid_argument("canvas requires code or script");
  Rectangle style = base_style(props.place, false);
  style.width = props.width;
  style.height = props.height;
  style.radius = props.radius;
  style.raster_visible = style.visible;
  style.color = {1.0F, 1.0F, 1.0F, std::clamp(props.opacity, 0.0F, 1.0F)};
  const std::string code = canvas_source(props);
  const std::size_t begin = scene_.rectangles.size();
  const auto [width, height] = add_canvas(scene_, props.id, code, style, props.animate);
  return push(begin, width, height, style, props.input);
}

void reset_text_field(std::string_view id) { text_field_states().erase(std::string(id)); }

Widget Ui::text_field(const TextFieldProps& props) {
  auto& states = text_field_states();
  if (props.id.empty()) throw std::invalid_argument("text_field id cannot be empty");
  if (props.authenticate && !scene_.session_lock)
    throw std::invalid_argument("authenticate text field requires a session lock");
  auto& state = states[props.id];
  if (!state) {
    state = std::make_shared<TextFieldState>();
    state->focused = props.authenticate || props.autofocus;
    state->value = props.value;
    state->caret = state->value.size();
  }
  // The value may have been replaced by an on_change handler since the last
  // build; keep the caret inside it and on a character boundary.
  std::string& value = state->value;
  const auto boundary = [&value](std::size_t offset) {
    offset = std::min(offset, value.size());
    while (offset > 0 && offset < value.size() &&
           (static_cast<unsigned char>(value[offset]) & 0xC0U) == 0x80U)
      --offset;
    return offset;
  };
  state->caret = boundary(state->caret);
  if (state->anchor) state->anchor = boundary(*state->anchor);

  Rectangle style = base_style(props.place, false);
  style.width = props.width;
  style.height = props.height;
  style.radius = props.radius;
  style.border_width = props.border_width;
  style.raster_visible = style.visible;
  style.color = clamped(props.color);
  style.color2 = style.color;
  const Color focus = clamped(props.focus_color);
  style.border_color = state->focused
      ? focus : clamped(props.border_color.value_or(Color{0.2F, 0.25F, 0.34F, 1.0F}));
  const std::size_t begin = scene_.rectangles.size();
  scene_.add(style);

  // What is drawn: the value, one bullet per character for passwords.
  std::vector<std::size_t> boundaries;  // value offsets of character boundaries
  std::string shown;
  for (std::size_t offset = 0; offset <= value.size(); ++offset) {
    if (offset == value.size() || (static_cast<unsigned char>(value[offset]) & 0xC0U) != 0x80U)
      boundaries.push_back(offset);
  }
  if (props.password) {
    for (std::size_t count = 1; count < boundaries.size(); ++count) shown += "•";
  } else {
    shown = value;
  }
  TextFieldLayout layout;
  layout.padding = props.padding;
  if (!shown.empty()) {
    const auto stops = caret_stops(shown, props.font, props.size, props.weight);
    for (std::size_t index = 0; index < stops.size() && index < boundaries.size(); ++index)
      layout.stops.emplace_back(boundaries[index], stops[index].x);
  } else {
    layout.stops.emplace_back(0, 0.0F);
  }
  const auto x_at = [&layout](std::size_t offset) {
    for (const auto& [stop, x] : layout.stops)
      if (stop == offset) return x;
    return 0.0F;
  };
  const float caret_x = x_at(state->caret);
  const float text_width = layout.stops.back().second;

  // Scroll horizontally so the caret stays inside the padded box.
  const float inner_x = style.x + props.padding;
  const float inner_width = std::max(1.0F, props.width - props.padding * 2.0F);
  float& scroll = state->scroll;
  if (!state->focused) scroll = 0.0F;
  if (caret_x - scroll > inner_width - 2.0F) scroll = caret_x - inner_width + 2.0F;
  if (caret_x - scroll < 0.0F) scroll = caret_x;
  scroll = std::clamp(scroll, 0.0F, std::max(0.0F, text_width - inner_width + 2.0F));

  // Everything inside the field is clipped to the padded box (and to any
  // clip the field itself has).
  const std::array<float, 4> inner_clip = intersect_clip(
      style.clip, {inner_x - 2.0F, style.y, inner_width + 4.0F, style.height});
  Rectangle content = style;
  content.radius = 0.0F;
  content.border_width = 0.0F;
  content.corner_radii = {-1.0F, -1.0F, -1.0F, -1.0F};
  content.clip = inner_clip;
  content.clip_radius = 0.0F;

  const auto center = [&](std::size_t first, float height) {
    const float dy = (style.height - height) * 0.5F;
    for (std::size_t index = first; index < scene_.rectangles.size(); ++index)
      scene_.rectangles[index].y += dy;
  };
  float line_height = props.size * 1.25F;
  const bool empty = value.empty();
  if (state->has_selection() && !empty && state->focused) {
    const float left = inner_x - scroll + x_at(state->selection_start());
    const float right = inner_x - scroll + x_at(state->selection_end());
    Rectangle selection = content;
    selection.x = std::max(inner_clip[0], left);
    selection.width = std::max(1.0F, std::min(right, inner_clip[0] + inner_clip[2]) - selection.x);
    selection.height = line_height;
    selection.y = style.y + (style.height - line_height) * 0.5F;
    selection.radius = 3.0F;
    selection.color = clamped(props.selection_color.value_or(
        Color{focus[0], focus[1], focus[2], focus[3] * 0.35F}));
    selection.color2 = selection.color;
    scene_.add(selection);
  }
  const std::string& text = empty ? props.placeholder : shown;
  if (!text.empty()) {
    Rectangle text_style = content;
    text_style.x = inner_x - (empty ? 0.0F : scroll);
    text_style.y = style.y;
    text_style.color = empty
        ? clamped(props.placeholder_color.value_or(Color{0.45F, 0.5F, 0.6F, 1.0F}))
        : clamped(props.text_color.value_or(Color{0.92F, 0.95F, 1.0F, 1.0F}));
    const std::size_t first = scene_.rectangles.size();
    const auto [ignored, height] = add_text(scene_, text, props.font, props.size, props.weight,
                                            text_style, TextOptions{0.0F, 1, "start", "none"});
    if (height > 0.0F) line_height = height;
    center(first, line_height);
  }
  if (state->focused) {
    Rectangle caret = content;
    caret.width = 2.0F;
    caret.height = line_height;
    caret.x = std::max(0.0F, inner_x + caret_x - scroll - 1.0F);
    caret.y = style.y + (style.height - line_height) * 0.5F;
    caret.radius = 1.0F;
    caret.color = clamped(props.caret_color.value_or(focus));
    caret.color2 = caret.color;
    scene_.add(caret);
  }

  TextField field;
  field.id = props.id;
  field.state = state;
  field.password = props.password;
  field.authenticate = props.authenticate;
  field.max_length = props.max_length;
  field.layout = std::move(layout);
  field.on_change = props.on_change;
  field.on_submit = props.on_submit;
  field.on_authenticated = props.on_authenticated;

  Interaction interaction;
  interaction.begin = begin;
  interaction.end = scene_.rectangles.size();
  interaction.x = style.x;
  interaction.y = style.y;
  interaction.width = style.width;
  interaction.height = style.height;
  interaction.enabled = style.visible;
  interaction.text_field = static_cast<int>(scene_.text_fields.size());
  interaction.clip = style.clip;
  field.interaction = scene_.interactions.size();
  scene_.interactions.push_back(std::move(interaction));
  scene_.text_fields.push_back(std::move(field));
  scene_.anchor = style.anchor;
  scene_.layer = style.layer;
  scene_.margin = style.margin;
  return Widget{begin, scene_.rectangles.size(), style.width, style.height,
                style.margin, style.anchor};
}

void Ui::clip(const Widget& widget, const Clip& clip) {
  if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.width) ||
      !std::isfinite(clip.height) || !std::isfinite(clip.radius))
    throw std::invalid_argument("clip requires finite values");
  const std::size_t end = std::min(widget.end, scene_.rectangles.size());
  clip_range(scene_, widget.begin, end,
             {clip.x, clip.y, std::max(0.0F, clip.width), std::max(0.0F, clip.height)},
             clip.radius > 0.0F ? std::optional<float>(clip.radius) : std::nullopt);
}

void Ui::fade(const Widget& widget, float opacity) {
  if (!std::isfinite(opacity)) throw std::invalid_argument("fade opacity must be finite");
  opacity = std::clamp(opacity, 0.0F, 1.0F);
  if (opacity >= 1.0F) return;
  const std::size_t end = std::min(widget.end, scene_.rectangles.size());
  // A raster's carrier colour alpha is its opacity, so this covers text and
  // images as well as fills, gradients and borders.
  for (std::size_t index = widget.begin; index < end; ++index) {
    Rectangle& rectangle = scene_.rectangles[index];
    rectangle.color[3] *= opacity;
    rectangle.color2[3] *= opacity;
    rectangle.border_color[3] *= opacity;
  }
}

Widget Ui::linear(std::span<const Widget> children, const LayoutProps& props, bool row) {
  if (children.empty()) throw std::invalid_argument("layout requires at least one child");
  const float gap = props.gap;
  const float padding = props.padding;
  float main_size = padding * 2.0F + gap * static_cast<float>(children.size() - 1);
  float cross_size = 0.0F;
  for (const Widget& child : children) {
    const float main_margin = row ? child.margin[3] + child.margin[1]
                                  : child.margin[0] + child.margin[2];
    const float cross_margin = row ? child.margin[0] + child.margin[2]
                                   : child.margin[3] + child.margin[1];
    main_size += (row ? child.width : child.height) + main_margin;
    cross_size = std::max(cross_size, (row ? child.height : child.width) + cross_margin);
  }
  const float width = props.width.value_or(row ? main_size : cross_size + padding * 2.0F);
  const float height = props.height.value_or(row ? cross_size + padding * 2.0F : main_size);
  float cursor = padding;
  for (const Widget& child : children) {
    const float available = (row ? height : width) - padding * 2.0F;
    const float child_cross = row ? child.height : child.width;
    const bool end = (child.anchor & (row ? anchor::bottom : anchor::right)) != 0U;
    const bool start = (child.anchor & (row ? anchor::top : anchor::left)) != 0U;
    const float cross = start ? padding + (row ? child.margin[0] : child.margin[3])
                              : end ? padding + available - child_cross -
                                          (row ? child.margin[2] : child.margin[1])
                                    : padding + (available - child_cross) * 0.5F;
    cursor += row ? child.margin[3] : child.margin[0];
    shift(scene_, child, row ? cursor : cross, row ? cross : cursor);
    cursor += (row ? child.width + child.margin[1] : child.height + child.margin[2]) + gap;
  }
  Rectangle style = base_style(props.place, true);
  style.clip = {0.0F, 0.0F, 0.0F, 0.0F};
  style.radius = props.radius;
  const Widget group{first_begin(children), scene_.rectangles.size(), width, height,
                     style.margin, style.anchor};
  if (!style.visible) hide_range(scene_, group);
  shift(scene_, group, style.x, style.y);
  const std::array<float, 4> parent_clip{style.x, style.y, width, height};
  for (std::size_t index = group.begin; index < group.end; ++index) {
    auto& rectangle = scene_.rectangles[index];
    rectangle.clip = parent_clip;
    rectangle.clip_radius = style.radius;
    rectangle.anchor = style.anchor;
    rectangle.layer = style.layer;
    rectangle.margin = style.margin;
    rectangle.exclusive = style.exclusive;
    rectangle.exclusive_zone = style.exclusive_zone;
  }
  grow_scene(scene_, style.x + width, style.y + height);
  Rectangle carrier = style;
  carrier.width = width;
  carrier.height = height;
  carrier.visible = false;
  scene_.add(carrier);
  return push(group.begin, width, height, style, props.input);
}

Widget Ui::row(std::span<const Widget> children, const LayoutProps& props) {
  return linear(children, props, true);
}

Widget Ui::column(std::span<const Widget> children, const LayoutProps& props) {
  return linear(children, props, false);
}

Widget Ui::stack(std::span<const Widget> children, const StackProps& props) {
  if (children.empty()) throw std::invalid_argument("stack requires at least one child");
  float measured_width = 0.0F;
  float measured_height = 0.0F;
  for (const Widget& child : children) {
    measured_width = std::max(measured_width, child.width + child.margin[3] + child.margin[1]);
    measured_height = std::max(measured_height, child.height + child.margin[0] + child.margin[2]);
  }
  const float width = props.width.value_or(measured_width);
  const float height = props.height.value_or(measured_height);
  Rectangle style = base_style(props.place, true);
  style.radius = props.radius;
  for (const Widget& child : children) {
    const bool left = (child.anchor & anchor::left) != 0U;
    const bool right = (child.anchor & anchor::right) != 0U;
    const bool top = (child.anchor & anchor::top) != 0U;
    const bool bottom = (child.anchor & anchor::bottom) != 0U;
    const float x = left ? child.margin[3]
                         : right ? width - child.width - child.margin[1]
                                 : (width - child.width) * 0.5F;
    const float y = top ? child.margin[0]
                        : bottom ? height - child.height - child.margin[2]
                                 : (height - child.height) * 0.5F;
    shift(scene_, child, style.x + x, style.y + y);
  }
  const std::size_t begin = first_begin(children);
  if (style.clip[2] > 0.0F && style.clip[3] > 0.0F)
    clip_range(scene_, begin, scene_.rectangles.size(), style.clip);
  if (!style.visible)
    hide_range(scene_, {begin, scene_.rectangles.size(), width, height, style.margin, style.anchor});
  Rectangle carrier = style;
  carrier.width = width;
  carrier.height = height;
  carrier.visible = false;
  scene_.add(carrier);
  grow_scene(scene_, style.x + width, style.y + height);
  return push(begin, width, height, style, props.input);
}

Widget Ui::finish_scroll(const Rectangle& style, std::size_t begin, float content_height,
                         float step, const std::shared_ptr<ScrollState>& state,
                         const Input& input) {
  const std::size_t end = scene_.rectangles.size();
  clip_range(scene_, begin, end, {style.x, style.y, style.width, style.height}, style.radius);
  if (!style.visible)
    hide_range(scene_, {begin, end, style.width, style.height, style.margin, style.anchor});
  Rectangle carrier = style;
  carrier.visible = false;
  const std::size_t carrier_index = scene_.rectangles.size();
  scene_.add(carrier);
  const float maximum = std::max(0.0F, content_height - style.height);
  if (style.visible)
    scene_.scroll_areas.push_back(ScrollArea{begin, carrier_index + 1, style.x, style.y,
                                             style.width, style.height, maximum, step, state,
                                             true});
  return push(begin, style.width, style.height, style, input);
}

Widget Ui::scrollbox(std::span<const Widget> children, const ScrollProps& props) {
  if (props.width <= 0.0F || props.height <= 0.0F)
    throw std::invalid_argument("scrollbox requires positive width and height");
  if (props.gap < 0.0F || props.padding < 0.0F)
    throw std::invalid_argument("scroll gap and padding must be non-negative");
  const Rectangle style = scroll_style(props.place, props.width, props.height, props.radius);
  const auto state = scroll_state(props.id);
  float content_height = props.padding * 2.0F;
  for (const Widget& child : children) content_height += child.height + child.margin[0] + child.margin[2];
  if (children.size() > 1) content_height += props.gap * static_cast<float>(children.size() - 1);
  state->offset = std::clamp(state->offset, 0.0F, std::max(0.0F, content_height - props.height));
  const std::size_t begin = children.empty() ? scene_.rectangles.size() : first_begin(children);
  float cursor = props.padding - state->offset;
  for (const Widget& child : children) {
    cursor += child.margin[0];
    shift(scene_, child, style.x + props.padding + child.margin[3], style.y + cursor);
    cursor += child.height + child.margin[2] + props.gap;
  }
  return finish_scroll(style, begin, content_height, props.scroll_step, state, props.input);
}

Widget Ui::listview(const ListviewProps& props) {
  if (props.width <= 0 || props.height <= 0 || props.gap < 0 ||
      (props.item_height <= 0 && !props.row_height))
    throw std::invalid_argument(
        "listview requires positive width, height, item_height (or row_height) and non-negative gap");
  if (!props.render) throw std::invalid_argument("listview requires a render function");
  const Rectangle style = scroll_style(props.place, props.width, props.height, props.radius);
  const auto state = scroll_state(props.id);
  const std::size_t count = props.count;
  // Row tops; fixed heights need no table.
  std::vector<float> tops;
  float content_height = 0.0F;
  const float stride = props.item_height + props.gap;
  if (props.row_height) {
    tops.reserve(count + 1);
    for (std::size_t index = 0; index < count; ++index) {
      tops.push_back(content_height);
      content_height += std::max(0.0F, props.row_height(index)) + props.gap;
    }
    tops.push_back(content_height);
    if (count > 0) content_height -= props.gap;
  } else {
    content_height = count == 0 ? 0.0F : static_cast<float>(count) * stride - props.gap;
  }
  const auto top = [&](std::size_t index) {
    return props.row_height ? tops[index] : static_cast<float>(index) * stride;
  };
  const auto bottom = [&](std::size_t index) {
    return props.row_height ? tops[index + 1] - props.gap : top(index) + props.item_height;
  };
  if (props.keep_visible != state->kept && props.keep_visible && *props.keep_visible < count) {
    state->kept = props.keep_visible;
    const std::size_t index = *props.keep_visible;
    if (top(index) < state->offset) state->offset = top(index);
    else if (bottom(index) > state->offset + props.height) state->offset = bottom(index) - props.height;
  }
  state->offset = std::clamp(state->offset, 0.0F, std::max(0.0F, content_height - props.height));
  // First and one-past-last rows that intersect the viewport.
  std::size_t first = 0;
  std::size_t last = 0;
  if (props.row_height) {
    first = static_cast<std::size_t>(std::upper_bound(tops.begin(), tops.end() - 1, state->offset) - tops.begin());
    first = first > 0 ? first - 1 : 0;
    last = first;
    while (last < count && top(last) < state->offset + props.height) ++last;
  } else {
    first = std::min(count, static_cast<std::size_t>(state->offset / stride));
    last = std::min(count, static_cast<std::size_t>(std::ceil((state->offset + props.height) / stride)) + 1);
  }
  std::size_t begin = scene_.rectangles.size();
  for (std::size_t index = first; index < last; ++index) {
    const std::optional<Widget> widget = props.render(index);
    if (!widget) continue;
    begin = std::min(begin, widget->begin);
    shift(scene_, *widget, style.x, style.y + top(index) - state->offset);
  }
  return finish_scroll(style, begin, content_height, props.scroll_step, state, props.input);
}

float scroll_offset(std::string_view id) { return scroll_state(std::string(id))->offset; }
void set_scroll_offset(std::string_view id, float offset) { scroll_state(std::string(id))->offset = offset; }

Widget Ui::anchor_layout(const AnchorLayoutProps& props) {
  const float width = props.width;
  const float height = props.height;
  if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0)
    throw std::invalid_argument("anchor_layout needs positive finite width and height");
  const Rectangle style = scroll_style(props.place, width, height, props.radius);
  const std::size_t begin = scene_.rectangles.size();
  for (const AnchorChild& child : props.children) {
    const auto& margin = child.margin;
    float w = child.left && child.right ? width - margin[3] - margin[1] : child.width.value_or(width);
    float h = child.top && child.bottom ? height - margin[0] - margin[2] : child.height.value_or(height);
    if (!std::isfinite(w) || !std::isfinite(h) || !std::isfinite(child.min_width) ||
        !std::isfinite(child.min_height) || !std::isfinite(child.max_width) ||
        !std::isfinite(child.max_height) || child.min_width <= 0 || child.min_height <= 0 ||
        child.max_width < child.min_width || child.max_height < child.min_height ||
        child.max_width > 8192 || child.max_height > 8192)
      throw std::invalid_argument("invalid anchor layout size constraints");
    if (!child.render) throw std::invalid_argument("anchor layout child requires render");
    w = std::clamp(w, child.min_width, child.max_width);
    h = std::clamp(h, child.min_height, child.max_height);
    const float x = child.left ? static_cast<float>(margin[3])
                  : child.right ? width - w - margin[1] : (width - w) / 2;
    const float y = child.top ? static_cast<float>(margin[0])
                  : child.bottom ? height - h - margin[2] : (height - h) / 2;
    const Widget widget = child.render(AnchorBox{x, y, w, h});
    if (widget.begin < begin) throw std::logic_error("anchor render must construct a new widget");
    shift(scene_, widget, style.x + x, style.y + y);
  }
  const std::size_t end = scene_.rectangles.size();
  clip_range(scene_, begin, end, {style.x, style.y, width, height});
  if (!style.visible) hide_range(scene_, {begin, end, width, height, style.margin, style.anchor});
  Rectangle carrier = style;
  carrier.visible = false;
  scene_.add(carrier);
  return push(begin, width, height, style, props.input);
}

void Ui::keyboard(KeyHandler handler, Keyboard mode) {
  scene_.on_key = std::move(handler);
  scene_.keyboard = mode;
}

void Ui::hide(const Widget& widget) { hide_range(scene_, widget); }

void Ui::move(const Widget& widget, float dx, float dy) { shift(scene_, widget, dx, dy); }

void Ui::surface(const SurfaceProps& props) {
  const Size configured = surface_size();
  // A fill axis takes the configured size; before the first configure the
  // drawn content's extent stands in until the compositor answers.
  const auto dimension = [](const std::optional<float>& requested, std::uint32_t content,
                            std::uint32_t configured_size, bool& fill) {
    fill = requested && *requested == 0.0F;
    if (fill) return std::max<std::uint32_t>(1, configured_size != 0 ? configured_size : content);
    return surface_dimension(requested.value_or(static_cast<float>(content)), "surface");
  };
  scene_.width = dimension(props.width, scene_.width, configured.width, scene_.fill_width);
  scene_.height = dimension(props.height, scene_.height, configured.height, scene_.fill_height);
  scene_.surface_configured = true;
  scene_.focus_grab = props.focus_grab;
  scene_.on_focus_activated = props.on_activated;
  scene_.on_focus_dismissed = props.on_dismiss;
  scene_.on_closed = props.on_closed;
  scene_.anchor = props.anchor | (scene_.fill_width ? anchor::left | anchor::right : 0U) |
                  (scene_.fill_height ? anchor::top | anchor::bottom : 0U);
  scene_.layer = static_cast<std::uint32_t>(props.layer);
  scene_.exclusive = props.exclusive_zone != 0;
  scene_.exclusive_zone = props.exclusive_zone;
  scene_.margin = props.margin;
  scene_.surface_namespace = props.surface_namespace;
  if (!std::isfinite(props.refresh_interval) || props.refresh_interval < 0.008 ||
      props.refresh_interval > 3600)
    throw std::invalid_argument("refresh_interval must be between 0.008 and 3600 seconds");
  scene_.refresh_interval = props.refresh_interval;
  scene_.selective_input = props.input_regions.has_value();
  scene_.input_regions.clear();
  if (props.input_regions) {
    for (const auto& region : *props.input_regions) {
      for (std::int32_t value : region)
        if (value < 0 || value > 8192) throw std::invalid_argument("input region is out of range");
      scene_.input_regions.push_back(region);
    }
  }
}

void Ui::toplevel(const ToplevelProps& props) {
  const Size current = toplevel_size();
  request_toplevel_size(
      surface_dimension(props.width.value_or(static_cast<float>(current.width)), "toplevel"),
      surface_dimension(props.height.value_or(static_cast<float>(current.height)), "toplevel"));
  if (!std::isfinite(props.refresh_interval) || props.refresh_interval < 0.008 ||
      props.refresh_interval > 3600)
    throw std::invalid_argument("refresh_interval must be between 0.008 and 3600 seconds");
  if (props.title.empty() || props.app_id.empty())
    throw std::invalid_argument("toplevel title and app_id cannot be empty");
  const Size size = toplevel_size();
  scene_.toplevel = true;
  scene_.refresh_interval = props.refresh_interval;
  scene_.toplevel_width = size.width;
  scene_.toplevel_height = size.height;
  scene_.title = props.title;
  scene_.app_id = props.app_id;
  scene_.on_closed = props.on_close;
}

void Ui::session_lock(const SessionLockProps& props) {
  if (scene_.toplevel) throw std::logic_error("session lock cannot be a toplevel");
  std::string service = props.pam_service;
  if (service.empty())
    service = std::filesystem::exists("/etc/pam.d/swaylock") ? "swaylock" : "login";
  if (!props.pam_conf.empty()) {
    if (props.pam_conf.front() != '/')
      throw std::invalid_argument("pam_conf must be an absolute file path");
    std::error_code error;
    if (!std::filesystem::is_regular_file(props.pam_conf, error))
      throw std::invalid_argument("pam_conf file is not readable: " + props.pam_conf);
  }
  scene_.session_lock = true;
  scene_.pam_service = service;
  scene_.pam_conf = props.pam_conf;
  scene_.on_closed = props.on_unlock;
  configure_session_authentication(std::move(service), props.pam_conf);
}

void Ui::lock_surface(const LockSurfaceProps& props) {
  if (!scene_.session_lock) throw std::logic_error("call session_lock before lock_surface");
  const LockSurfaceContext context = lock_surface_context();
  const std::uint32_t width = surface_dimension(
      props.width.value_or(static_cast<float>(context.width)), "lock surface");
  const std::uint32_t height = surface_dimension(
      props.height.value_or(static_cast<float>(context.height)), "lock surface");
  scene_.lock_surface = true;
  scene_.toplevel_width = context.output.empty() ? width : context.width;
  scene_.toplevel_height = context.output.empty() ? height : context.height;
  scene_.lock_output = context.output;
}

void Ui::finish() {
  Scene& scene = scene_;
  if (scene.rectangles.empty() && !scene.toplevel && !scene.session_lock)
    throw std::runtime_error("the scene did not create any widgets");
  if (scene.focus_grab && (scene.toplevel || scene.session_lock))
    throw std::runtime_error("focus_grab requires a layer-shell surface");
  if (!scene.scroll_areas.empty() && !scene.surface_configured) {
    scene.width = 1;
    scene.height = 1;
    for (const Rectangle& rectangle : scene.rectangles) {
      if (!rectangle.visible && !rectangle.raster_visible) continue;
      float right = rectangle.x + rectangle.width;
      float bottom = rectangle.y + rectangle.height;
      if (rectangle.clip[2] > 0.0F && rectangle.clip[3] > 0.0F) {
        right = std::min(right, rectangle.clip[0] + rectangle.clip[2]);
        bottom = std::min(bottom, rectangle.clip[1] + rectangle.clip[3]);
      }
      grow_scene(scene, std::max(0.0F, right), std::max(0.0F, bottom));
    }
    for (const ScrollArea& area : scene.scroll_areas)
      grow_scene(scene, area.x + area.width, area.y + area.height);
  }
  if (scene.toplevel) {
    scene.width = scene.toplevel_width;
    scene.height = scene.toplevel_height;
  }
  if (scene.session_lock) {
    if (!scene.lock_surface) throw std::runtime_error("session lock requires a lock surface");
    scene.width = scene.toplevel_width;
    scene.height = scene.toplevel_height;
  }
}

}  // namespace vw
