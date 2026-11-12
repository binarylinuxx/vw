#include "vw/canvas.hpp"
#include "vw/animation.hpp"

#include <quickjs.h>
#include <cairo.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace vw {
namespace {
using Clock = std::chrono::steady_clock;
enum Operation {
  fill_rect, stroke_rect, clear_rect, begin_path, close_path, move_to, line_to,
  arc, bezier_curve, quadratic_curve, path_rect, fill, stroke, clip,
  save, restore, translate, rotate, scale, set_transform, transform,
  reset_transform, fill_text, stroke_text, measure_text, log,
};
constexpr const char* bootstrap = R"JS(
ctx.fillStyle = '#000000'; ctx.strokeStyle = '#000000';
ctx.globalAlpha = 1; ctx.lineWidth = 1;
ctx.lineCap = 'butt'; ctx.lineJoin = 'miter'; ctx.miterLimit = 10;
ctx.font = '10px sans-serif'; ctx.textAlign = 'start'; ctx.textBaseline = 'alphabetic';
ctx.globalCompositeOperation = 'source-over';
(() => {
  const nativeSave = ctx.save.bind(ctx), nativeRestore = ctx.restore.bind(ctx);
  const keys = ['fillStyle', 'strokeStyle', 'globalAlpha', 'lineWidth', 'lineCap',
                'lineJoin', 'miterLimit', 'font', 'textAlign', 'textBaseline', 'globalCompositeOperation'];
  const stack = [];
  ctx.save = function() {
    stack.push(keys.map(key => ctx[key]));
    nativeSave();
  };
  ctx.restore = function() {
    if (!stack.length) return;
    const values = stack.pop();
    keys.forEach((key, index) => ctx[key] = values[index]);
    nativeRestore();
  };
  Object.defineProperty(ctx, 'canvas', {value: canvas});
  canvas.getContext = function(kind) {
    if (kind !== '2d') throw new TypeError('Only a 2d context is supported');
    return ctx;
  };
})();
)JS";

struct Bitmap { std::uint32_t width, height; Pixels pixels; };

class Canvas {
 public:
  Canvas(std::string code, std::string id, std::uint32_t width, std::uint32_t height)
      : code_(std::move(code)), id_(std::move(id)), width_(width), height_(height) {
    runtime_ = JS_NewRuntime();
    if (runtime_ == nullptr) throw std::runtime_error("could not create JavaScript runtime");
    JS_SetMemoryLimit(runtime_, 32 * 1024 * 1024);
    JS_SetMaxStackSize(runtime_, 512 * 1024);
    JS_SetInterruptHandler(runtime_, [](JSRuntime*, void* opaque) {
      return Clock::now() > static_cast<Canvas*>(opaque)->deadline_ ? 1 : 0;
    }, this);
    context_ = JS_NewContext(runtime_);
    if (context_ == nullptr) {
      JS_FreeRuntime(runtime_);
      runtime_ = nullptr;
      throw std::runtime_error("could not create JavaScript context");
    }
    JS_SetContextOpaque(context_, this);
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_ = cairo_create(surface_);
    try {
      check_cairo();
      cairo_font_options_t* font_options = cairo_font_options_create();
      cairo_font_options_set_antialias(font_options, CAIRO_ANTIALIAS_GRAY);
      cairo_set_font_options(cairo_, font_options);
      cairo_font_options_destroy(font_options);
      initialize();
    } catch (...) { cleanup(); throw; }
  }
  ~Canvas() { cleanup(); }
  bool matches(std::string_view code, std::uint32_t width, std::uint32_t height) const {
    return code_ == code && width_ == width && height_ == height;
  }
  const Bitmap& draw(bool animate) {
    if (bitmap_ && !animate) return *bitmap_;
    deadline_ = Clock::now() + std::chrono::milliseconds(100);
    JSValue global = JS_GetGlobalObject(context_);
    JSValue function = JS_GetPropertyStr(context_, global, "draw");
    if (JS_IsFunction(context_, function)) {
      JSValue arguments[] = {
        JS_GetPropertyStr(context_, global, "ctx"),
        JS_GetPropertyStr(context_, global, "canvas"),
        JS_NewFloat64(context_, std::chrono::duration<double>(Clock::now() - started_).count()),
      };
      JSValue result = JS_Call(context_, function, global, 3, arguments);
      for (auto argument : arguments) JS_FreeValue(context_, argument);
      JS_FreeValue(context_, function);
      JS_FreeValue(context_, global);
      if (JS_IsException(result)) throw std::runtime_error(exception());
      JS_FreeValue(context_, result);
    } else {
      JS_FreeValue(context_, function);
      JS_FreeValue(context_, global);
      if (animate) throw std::runtime_error("animated canvas requires function draw(ctx, canvas, time)");
    }
    check_cairo();
    cairo_surface_flush(surface_);
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width_) * height_);
    const unsigned char* source = cairo_image_surface_get_data(surface_);
    const int stride = cairo_image_surface_get_stride(surface_);
    for (std::uint32_t y = 0; y < height_; ++y) for (std::uint32_t x = 0; x < width_; ++x) {
      const auto* pixel = source + y * stride + x * 4;
      pixels[static_cast<std::size_t>(y) * width_ + x] =
          static_cast<std::uint32_t>(pixel[2]) | static_cast<std::uint32_t>(pixel[1]) << 8U |
          static_cast<std::uint32_t>(pixel[0]) << 16U | static_cast<std::uint32_t>(pixel[3]) << 24U;
    }
    bitmap_ = std::make_unique<Bitmap>(Bitmap{
        width_, height_, std::make_shared<const std::vector<std::uint32_t>>(std::move(pixels))});
    return *bitmap_;
  }

 private:
  void cleanup() {
    if (context_ != nullptr) { JS_FreeContext(context_); context_ = nullptr; }
    if (runtime_ != nullptr) { JS_FreeRuntime(runtime_); runtime_ = nullptr; }
    if (cairo_ != nullptr) { cairo_destroy(cairo_); cairo_ = nullptr; }
    if (surface_ != nullptr) { cairo_surface_destroy(surface_); surface_ = nullptr; }
  }
  void check_cairo() const {
    const auto status = cairo_status(cairo_);
    if (status != CAIRO_STATUS_SUCCESS)
      throw std::runtime_error(std::string("canvas drawing failed: ") + cairo_status_to_string(status));
  }
  std::string exception() {
    JSValue error = JS_GetException(context_);
    const char* message = JS_ToCString(context_, error);
    std::string result = "canvas " + id_ + ": " + (message == nullptr ? "JavaScript error" : message);
    if (message != nullptr) JS_FreeCString(context_, message);
    JS_FreeValue(context_, error);
    return result;
  }
  void evaluate(const char* source, std::size_t length, const char* filename) {
    deadline_ = Clock::now() + std::chrono::milliseconds(100);
    JSValue result = JS_Eval(context_, source, length, filename, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) throw std::runtime_error(exception());
    JS_FreeValue(context_, result);
  }
  void initialize() {
    JSValue global = JS_GetGlobalObject(context_);
    JSValue object = JS_NewObject(context_);
    const struct { const char* name; int operation; int arguments; } methods[] = {
      {"fillRect", fill_rect, 4}, {"strokeRect", stroke_rect, 4}, {"clearRect", clear_rect, 4},
      {"beginPath", begin_path, 0}, {"closePath", close_path, 0}, {"moveTo", move_to, 2},
      {"lineTo", line_to, 2}, {"arc", arc, 5}, {"bezierCurveTo", bezier_curve, 6},
      {"quadraticCurveTo", quadratic_curve, 4}, {"rect", path_rect, 4}, {"fill", fill, 0},
      {"stroke", stroke, 0}, {"clip", clip, 0}, {"save", save, 0}, {"restore", restore, 0},
      {"translate", translate, 2}, {"rotate", rotate, 1}, {"scale", scale, 2},
      {"setTransform", set_transform, 6}, {"transform", transform, 6},
      {"resetTransform", reset_transform, 0}, {"fillText", fill_text, 3},
      {"strokeText", stroke_text, 3}, {"measureText", measure_text, 1},
    };
    for (const auto& method : methods)
      JS_SetPropertyStr(context_, object, method.name, JS_NewCFunctionMagic(context_, host,
          method.name, method.arguments, JS_CFUNC_generic_magic, method.operation));
    JS_SetPropertyStr(context_, global, "ctx", object);
    JSValue canvas = JS_NewObject(context_);
    JS_DefinePropertyValueStr(context_, canvas, "width", JS_NewUint32(context_, width_), JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(context_, canvas, "height", JS_NewUint32(context_, height_), JS_PROP_ENUMERABLE);
    JS_SetPropertyStr(context_, global, "canvas", canvas);
    JSValue console = JS_NewObject(context_);
    JS_SetPropertyStr(context_, console, "log", JS_NewCFunctionMagic(context_, host,
        "log", 1, JS_CFUNC_generic_magic, log));
    JS_SetPropertyStr(context_, global, "console", console);
    JS_FreeValue(context_, global);
    evaluate(bootstrap, std::char_traits<char>::length(bootstrap), "canvas-runtime.js");
    evaluate(code_.c_str(), code_.size(), id_.c_str());
  }
  static std::string string(JSContext* context, JSValueConst value) {
    const char* text = JS_ToCString(context, value);
    if (text == nullptr) throw std::invalid_argument("expected a string");
    std::string result(text);
    JS_FreeCString(context, text);
    return result;
  }
  std::string property(JSValueConst object, const char* name) {
    JSValue value = JS_GetPropertyStr(context_, object, name);
    std::string result;
    try { result = string(context_, value); }
    catch (...) { JS_FreeValue(context_, value); throw; }
    JS_FreeValue(context_, value);
    return result;
  }
  double number_property(JSValueConst object, const char* name) {
    JSValue value = JS_GetPropertyStr(context_, object, name);
    double result = 0;
    const int status = JS_ToFloat64(context_, &result, value);
    JS_FreeValue(context_, value);
    if (status < 0 || !std::isfinite(result)) throw std::invalid_argument("expected a finite numeric property");
    return result;
  }
  void paint(JSValueConst object, bool outline) {
    GdkRGBA color;
    const auto style = property(object, outline ? "strokeStyle" : "fillStyle");
    if (!gdk_rgba_parse(&color, style.c_str())) throw std::invalid_argument("unsupported canvas color: " + style);
    const double alpha = std::clamp(number_property(object, "globalAlpha"), 0.0, 1.0);
    cairo_set_source_rgba(cairo_, color.red, color.green, color.blue, color.alpha * alpha);
    const auto composite = property(object, "globalCompositeOperation");
    cairo_operator_t mode;
    if (composite == "source-over") mode = CAIRO_OPERATOR_OVER;
    else if (composite == "copy") mode = CAIRO_OPERATOR_SOURCE;
    else if (composite == "destination-over") mode = CAIRO_OPERATOR_DEST_OVER;
    else if (composite == "lighter") mode = CAIRO_OPERATOR_ADD;
    else if (composite == "multiply") mode = CAIRO_OPERATOR_MULTIPLY;
    else if (composite == "screen") mode = CAIRO_OPERATOR_SCREEN;
    else throw std::invalid_argument("unsupported canvas composite mode: " + composite);
    cairo_set_operator(cairo_, mode);
    if (outline) {
      const double width = number_property(object, "lineWidth");
      const double limit = number_property(object, "miterLimit");
      if (width <= 0 || limit <= 0) throw std::invalid_argument("lineWidth and miterLimit must be positive");
      cairo_set_line_width(cairo_, width);
      cairo_set_miter_limit(cairo_, limit);
      const auto cap = property(object, "lineCap"), join = property(object, "lineJoin");
      cairo_set_line_cap(cairo_, cap == "round" ? CAIRO_LINE_CAP_ROUND :
                                cap == "square" ? CAIRO_LINE_CAP_SQUARE : CAIRO_LINE_CAP_BUTT);
      cairo_set_line_join(cairo_, join == "round" ? CAIRO_LINE_JOIN_ROUND :
                                 join == "bevel" ? CAIRO_LINE_JOIN_BEVEL : CAIRO_LINE_JOIN_MITER);
    }
  }
  PangoLayout* text_layout(JSValueConst object, const std::string& text) {
    const auto font = property(object, "font");
    static const std::regex pattern(R"(^\s*(.*?)\s*([0-9]+(?:\.[0-9]+)?)px\s+(.+)$)");
    std::smatch match;
    if (!std::regex_match(font, match, pattern)) throw std::invalid_argument("canvas font must use pixels, e.g. '16px sans-serif'");
    const double size = std::stod(match[2].str());
    if (size <= 0 || size > 4096) throw std::invalid_argument("canvas font size is out of range");
    const auto style = match[1].str();
    std::string family = match[3].str();
    if (family.size() > 1 && (family.front() == '\'' || family.front() == '"') && family.back() == family.front())
      family = family.substr(1, family.size() - 2);
    PangoFontDescription* description = pango_font_description_new();
    pango_font_description_set_family(description, family.c_str());
    pango_font_description_set_absolute_size(description, size * PANGO_SCALE);
    if (style.find("bold") != std::string::npos) pango_font_description_set_weight(description, PANGO_WEIGHT_BOLD);
    if (style.find("italic") != std::string::npos) pango_font_description_set_style(description, PANGO_STYLE_ITALIC);
    PangoLayout* layout = pango_cairo_create_layout(cairo_);
    pango_layout_set_font_description(layout, description);
    pango_layout_set_text(layout, text.c_str(), static_cast<int>(text.size()));
    pango_font_description_free(description);
    return layout;
  }
  static JSValue host(JSContext* context, JSValueConst object, int argc,
                       JSValueConst* argv, int operation) {
    auto* self = static_cast<Canvas*>(JS_GetContextOpaque(context));
    try {
      const auto number = [&](int index) {
        double result = 0;
        if (index >= argc || JS_ToFloat64(context, &result, argv[index]) < 0 || !std::isfinite(result))
          throw std::invalid_argument("canvas method needs finite numeric arguments");
        return result;
      };
      cairo_t* cairo = self->cairo_;
      switch (operation) {
        case log:
          for (int index = 0; index < argc; ++index) std::cerr << (index == 0 ? "canvas: " : " ") << string(context, argv[index]);
          std::cerr << '\n'; break;
        case begin_path: cairo_new_path(cairo); break;
        case close_path: cairo_close_path(cairo); break;
        case move_to: cairo_move_to(cairo, number(0), number(1)); break;
        case line_to: cairo_line_to(cairo, number(0), number(1)); break;
        case path_rect: cairo_rectangle(cairo, number(0), number(1), number(2), number(3)); break;
        case bezier_curve: cairo_curve_to(cairo, number(0), number(1), number(2), number(3), number(4), number(5)); break;
        case quadratic_curve: {
          const double cx = number(0), cy = number(1), x = number(2), y = number(3);
          if (!cairo_has_current_point(cairo)) cairo_move_to(cairo, cx, cy);
          double px = 0, py = 0; cairo_get_current_point(cairo, &px, &py);
          cairo_curve_to(cairo, px + (cx - px) * 2 / 3, py + (cy - py) * 2 / 3,
                          x + (cx - x) * 2 / 3, y + (cy - y) * 2 / 3, x, y);
          break;
        }
        case arc: {
          const double x = number(0), y = number(1), radius = number(2), start = number(3), end = number(4);
          if (radius < 0) throw std::invalid_argument("arc radius cannot be negative");
          const bool reverse = argc > 5 && JS_ToBool(context, argv[5]) > 0;
          if (reverse) cairo_arc_negative(cairo, x, y, radius, start, end);
          else cairo_arc(cairo, x, y, radius, start, end);
          break;
        }
        case fill: case clip: {
          const auto rule = argc > 0 ? string(context, argv[0]) : "nonzero";
          if (rule != "nonzero" && rule != "evenodd") throw std::invalid_argument("unsupported fill rule");
          cairo_set_fill_rule(cairo, rule == "evenodd" ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING);
          if (operation == fill) { self->paint(object, false); cairo_fill_preserve(cairo); }
          else cairo_clip_preserve(cairo);
          break;
        }
        case stroke: self->paint(object, true); cairo_stroke_preserve(cairo); break;
        case fill_rect: case stroke_rect: case clear_rect: {
          const double x = number(0), y = number(1), w = number(2), h = number(3);
          if (operation != clear_rect) self->paint(object, operation == stroke_rect);
          cairo_path_t* path = cairo_copy_path(cairo);
          cairo_new_path(cairo); cairo_rectangle(cairo, x, y, w, h);
          if (operation == clear_rect) {
            cairo_save(cairo); cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR); cairo_fill(cairo); cairo_restore(cairo);
          } else if (operation == fill_rect) cairo_fill(cairo);
          else cairo_stroke(cairo);
          cairo_new_path(cairo); cairo_append_path(cairo, path); cairo_path_destroy(path);
          break;
        }
        case save: cairo_save(cairo); break;
        case restore: cairo_restore(cairo); break;
        case translate: cairo_translate(cairo, number(0), number(1)); break;
        case rotate: cairo_rotate(cairo, number(0)); break;
        case scale: {
          const double x = number(0), y = number(1);
          if (x == 0 || y == 0) throw std::invalid_argument("singular canvas scale");
          cairo_scale(cairo, x, y); break;
        }
        case reset_transform: cairo_identity_matrix(cairo); break;
        case set_transform: case transform: {
          const double a = number(0), b = number(1), c = number(2), d = number(3), e = number(4), f = number(5);
          if (std::abs(a * d - b * c) < 1e-12) throw std::invalid_argument("singular canvas transform");
          cairo_matrix_t matrix; cairo_matrix_init(&matrix, a, b, c, d, e, f);
          if (operation == transform) cairo_transform(cairo, &matrix);
          else cairo_set_matrix(cairo, &matrix);
          break;
        }
        case fill_text: case stroke_text: case measure_text: {
          if (argc < 1) throw std::invalid_argument("text method requires a string");
          const auto content = string(context, argv[0]);
          const double x = operation == measure_text ? 0 : number(1);
          const double y = operation == measure_text ? 0 : number(2);
          if (operation != measure_text) self->paint(object, operation == stroke_text);
          PangoLayout* layout = self->text_layout(object, content);
          int w = 0, h = 0; pango_layout_get_pixel_size(layout, &w, &h);
          if (operation == measure_text) {
            g_object_unref(layout);
            JSValue result = JS_NewObject(context);
            JS_SetPropertyStr(context, result, "width", JS_NewFloat64(context, w));
            return result;
          }
          const auto align = self->property(object, "textAlign");
          const auto baseline = self->property(object, "textBaseline");
          const double left = align == "center" ? x - w / 2.0 :
                              align == "right" || align == "end" ? x - w : x;
          const double top = baseline == "top" ? y : baseline == "middle" ? y - h / 2.0 :
                             baseline == "bottom" ? y - h : y - pango_layout_get_baseline(layout) / static_cast<double>(PANGO_SCALE);
          cairo_path_t* path = cairo_copy_path(cairo);
          cairo_new_path(cairo); cairo_move_to(cairo, left, top);
          pango_cairo_layout_path(cairo, layout);
          if (operation == fill_text) cairo_fill(cairo); else cairo_stroke(cairo);
          cairo_new_path(cairo); cairo_append_path(cairo, path); cairo_path_destroy(path);
          g_object_unref(layout);
          break;
        }
        default: throw std::invalid_argument("unknown canvas operation");
      }
      self->check_cairo();
      return JS_UNDEFINED;
    } catch (const std::exception& error) {
      return JS_ThrowTypeError(context, "%s", error.what());
    }
  }

  std::string code_, id_;
  std::uint32_t width_, height_;
  JSRuntime* runtime_ = nullptr;
  JSContext* context_ = nullptr;
  cairo_surface_t* surface_ = nullptr;
  cairo_t* cairo_ = nullptr;
  Clock::time_point started_ = Clock::now();
  Clock::time_point deadline_{};
  std::unique_ptr<Bitmap> bitmap_;
};
}  // namespace

std::pair<float, float> add_canvas(Scene& scene, std::string_view id,
                                  std::string_view code, const Rectangle& style,
                                  bool animate) {
  if (id.empty() || code.empty()) throw std::invalid_argument("canvas requires an id and JavaScript code");
  if (!std::isfinite(style.width) || !std::isfinite(style.height) ||
      style.width < 1 || style.height < 1 || style.width > 8192 || style.height > 8192)
    throw std::invalid_argument("canvas dimensions must be between 1 and 8192");
  static std::mutex mutex;
  static std::unordered_map<std::string, std::unique_ptr<Canvas>> cache;
  const auto width = static_cast<std::uint32_t>(std::ceil(style.width));
  const auto height = static_cast<std::uint32_t>(std::ceil(style.height));
  std::lock_guard lock(mutex);
  auto found = cache.find(std::string(id));
  if (found == cache.end() || !found->second->matches(code, width, height)) {
    auto canvas = std::make_unique<Canvas>(std::string(code), std::string(id), width, height);
    if (found == cache.end() && cache.size() >= 64) cache.clear();
    cache[std::string(id)] = std::move(canvas);
  }
  const bool animated = animate && style.raster_visible;
  const Bitmap& bitmap = cache.at(std::string(id))->draw(animated);
  Rectangle carrier = style;
  carrier.visible = false;
  carrier.width = static_cast<float>(width);
  carrier.height = static_cast<float>(height);
  const auto index = scene.rectangles.size();
  scene.add(carrier);
  scene.rasters.push_back(Raster{index, width, height, bitmap.pixels, false, false, true});
  if (animated) request_animation_continuation();
  return {static_cast<float>(width), static_cast<float>(height)};
}
}  // namespace vw
