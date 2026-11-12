#include "vw/text.hpp"

#include <cairo.h>
#include <fontconfig/fontconfig.h>
#include <hb-ot.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vw {
namespace {

constexpr std::size_t max_cache_entries = 512;
constexpr std::size_t max_cache_bytes = 32 * 1024 * 1024;
constexpr int hard_max_lines = 500;
constexpr float text_scale = 1.0F;

enum class TextAlign : std::uint8_t { start, center, end };
enum class TextEllipsize : std::uint8_t { none, start, middle, end };

struct TextMetrics {
  float width = 0.0F;
  float top = 0.0F;
  float bottom = 0.0F;
};

struct TextBitmap {
  std::uint32_t width = 1;
  std::uint32_t height = 1;
  float offset_x = 0.0F;
  float offset_y = 0.0F;
  TextMetrics metrics;
  Pixels pixels;
  bool alpha_mask = true;
};

struct TextKey {
  std::string text;
  std::string family;
  std::uint32_t size_bits = 0;
  std::uint32_t max_width_bits = 0;
  std::uint32_t color_rgb = 0;
  int weight = 0;
  int max_lines = 0;
  TextAlign align = TextAlign::start;
  TextEllipsize ellipsize = TextEllipsize::end;

  bool operator==(const TextKey&) const = default;
};

void hash_combine(std::size_t& seed, std::size_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 12U) + (seed >> 4U);
}

struct TextKeyHash {
  std::size_t operator()(const TextKey& key) const noexcept {
    std::size_t seed = 0x7e4b2a9c5d3f8161ULL;
    hash_combine(seed, std::hash<std::string>{}(key.text));
    hash_combine(seed, std::hash<std::string>{}(key.family));
    hash_combine(seed, key.size_bits);
    hash_combine(seed, key.max_width_bits);
    hash_combine(seed, key.color_rgb);
    hash_combine(seed, static_cast<std::size_t>(key.weight));
    hash_combine(seed, static_cast<std::size_t>(key.max_lines));
    hash_combine(seed, static_cast<std::size_t>(key.align));
    hash_combine(seed, static_cast<std::size_t>(key.ellipsize));
    return seed;
  }
};

struct CacheEntry {
  std::shared_ptr<const TextBitmap> bitmap;
  std::list<TextKey>::iterator lru;
  std::size_t bytes = 0;
};

struct VerticalExtents {
  float top = 0.0F;
  float bottom = 0.0F;
  bool valid = false;
};

std::uint8_t channel(float value) {
  return static_cast<std::uint8_t>(
      std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

std::uint32_t pack_rgb(const std::array<float, 4>& color) {
  return static_cast<std::uint32_t>(channel(color[0])) << 16U |
         static_cast<std::uint32_t>(channel(color[1])) << 8U |
         static_cast<std::uint32_t>(channel(color[2]));
}

TextAlign parse_align(std::string_view value) {
  if (value == "start" || value == "left") return TextAlign::start;
  if (value == "center") return TextAlign::center;
  if (value == "end" || value == "right") return TextAlign::end;
  throw std::invalid_argument("text align must be start, center, or end");
}

TextEllipsize parse_ellipsize(std::string_view value) {
  if (value == "none") return TextEllipsize::none;
  if (value == "start") return TextEllipsize::start;
  if (value == "middle") return TextEllipsize::middle;
  if (value == "end") return TextEllipsize::end;
  throw std::invalid_argument("text ellipsize must be none, start, middle, or end");
}

PangoAlignment pango_align(TextAlign align) {
  if (align == TextAlign::center) return PANGO_ALIGN_CENTER;
  if (align == TextAlign::end) return PANGO_ALIGN_RIGHT;
  return PANGO_ALIGN_LEFT;
}

PangoEllipsizeMode pango_ellipsize(TextEllipsize ellipsize) {
  if (ellipsize == TextEllipsize::none) return PANGO_ELLIPSIZE_NONE;
  if (ellipsize == TextEllipsize::start) return PANGO_ELLIPSIZE_START;
  if (ellipsize == TextEllipsize::middle) return PANGO_ELLIPSIZE_MIDDLE;
  return PANGO_ELLIPSIZE_END;
}

PangoWeight pango_weight(int weight) {
  if (weight >= FC_WEIGHT_BLACK) return PANGO_WEIGHT_HEAVY;
  if (weight >= FC_WEIGHT_EXTRABOLD) return PANGO_WEIGHT_ULTRABOLD;
  if (weight >= FC_WEIGHT_BOLD) return PANGO_WEIGHT_BOLD;
  if (weight >= FC_WEIGHT_DEMIBOLD) return PANGO_WEIGHT_SEMIBOLD;
  if (weight >= FC_WEIGHT_MEDIUM) return PANGO_WEIGHT_MEDIUM;
  if (weight <= FC_WEIGHT_THIN) return PANGO_WEIGHT_THIN;
  if (weight <= FC_WEIGHT_EXTRALIGHT) return PANGO_WEIGHT_ULTRALIGHT;
  if (weight <= FC_WEIGHT_LIGHT) return PANGO_WEIGHT_LIGHT;
  return PANGO_WEIGHT_NORMAL;
}

bool contains_color_glyph(std::string_view text) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
  for (std::size_t index = 0; index < text.size();) {
    const unsigned char first = bytes[index];
    char32_t codepoint = 0;
    std::size_t length = 1;
    if (first < 0x80U) {
      codepoint = first;
    } else if ((first & 0xe0U) == 0xc0U && index + 1 < text.size()) {
      codepoint = static_cast<char32_t>((first & 0x1fU) << 6U |
                                        (bytes[index + 1] & 0x3fU));
      length = 2;
    } else if ((first & 0xf0U) == 0xe0U && index + 2 < text.size()) {
      codepoint = static_cast<char32_t>((first & 0x0fU) << 12U |
                                        (bytes[index + 1] & 0x3fU) << 6U |
                                        (bytes[index + 2] & 0x3fU));
      length = 3;
    } else if ((first & 0xf8U) == 0xf0U && index + 3 < text.size()) {
      codepoint = static_cast<char32_t>((first & 0x07U) << 18U |
                                        (bytes[index + 1] & 0x3fU) << 12U |
                                        (bytes[index + 2] & 0x3fU) << 6U |
                                        (bytes[index + 3] & 0x3fU));
      length = 4;
    } else {
      return true;
    }
    index += length;
    if ((codepoint >= 0x2600 && codepoint <= 0x27bf) ||
        (codepoint >= 0x1f000 && codepoint <= 0x1ffff)) return true;
  }
  return false;
}

VerticalExtents clipping_extents(PangoFont* font) {
  if (font == nullptr) return {};
  hb_font_t* harfbuzz = pango_font_get_hb_font(font);
  if (harfbuzz == nullptr) return {};
  hb_position_t ascent = 0;
  hb_position_t descent = 0;
  hb_ot_metrics_get_position_with_fallback(
      harfbuzz, HB_OT_METRICS_TAG_HORIZONTAL_CLIPPING_ASCENT, &ascent);
  hb_ot_metrics_get_position_with_fallback(
      harfbuzz, HB_OT_METRICS_TAG_HORIZONTAL_CLIPPING_DESCENT, &descent);
  constexpr float units = 1.0F / (static_cast<float>(PANGO_SCALE) * text_scale);
  const float top = -static_cast<float>(ascent) * units;
  const float bottom = std::abs(static_cast<float>(descent) * units);
  if (!std::isfinite(top) || !std::isfinite(bottom) || bottom <= top) return {};
  return {top, bottom, true};
}

VerticalExtents clipping_extents(PangoLayout* layout) {
  if (pango_layout_get_line_count(layout) != 1) return {};
  VerticalExtents result;
  PangoLayoutIter* iter = pango_layout_get_iter(layout);
  do {
    PangoLayoutRun* run = pango_layout_iter_get_run_readonly(iter);
    if (run == nullptr || run->item == nullptr) continue;
    const VerticalExtents current = clipping_extents(run->item->analysis.font);
    if (!current.valid) continue;
    if (!result.valid) result = current;
    else {
      result.top = std::min(result.top, current.top);
      result.bottom = std::max(result.bottom, current.bottom);
    }
  } while (pango_layout_iter_next_run(iter));
  pango_layout_iter_free(iter);
  return result;
}

class TextRenderer {
 public:
  TextRenderer() {
    if (!FcInit()) throw std::runtime_error("fontconfig initialization failed");
    font_map_ = pango_cairo_font_map_new();
    if (font_map_ == nullptr) throw std::runtime_error("could not create Pango font map");
    context_ = pango_font_map_create_context(font_map_);
    if (context_ == nullptr) throw std::runtime_error("could not create Pango context");
    pango_context_set_base_dir(context_, PANGO_DIRECTION_LTR);
    cairo_font_options_set_antialias(font_options_, CAIRO_ANTIALIAS_GRAY);
    pango_cairo_context_set_font_options(context_, font_options_);
    cache_.max_load_factor(1.0F);
    cache_.reserve(max_cache_entries + 16);
  }

  ~TextRenderer() {
    cache_.clear();
    lru_.clear();
    if (context_ != nullptr) g_object_unref(context_);
    if (font_map_ != nullptr) g_object_unref(font_map_);
    cairo_font_options_destroy(font_options_);
    FcFini();
  }

  TextRenderer(const TextRenderer&) = delete;
  TextRenderer& operator=(const TextRenderer&) = delete;

  std::shared_ptr<const TextBitmap> render(const TextKey& key, float size,
                                           float max_width,
                                           const std::array<float, 4>& color) {
    std::lock_guard lock(mutex_);
    if (const auto found = cache_.find(key); found != cache_.end()) {
      lru_.splice(lru_.begin(), lru_, found->second.lru);
      return found->second.bitmap;
    }

    PangoLayout* layout = build_layout(key, size, max_width);
    auto bitmap = rasterize(layout, color, key.color_rgb == 0U);
    g_object_unref(layout);

    lru_.push_front(key);
    const std::size_t bytes = bitmap->pixels->size() * sizeof(std::uint32_t);
    cache_.emplace(key, CacheEntry{bitmap, lru_.begin(), bytes});
    cache_bytes_ += bytes;
    evict_if_needed();
    return bitmap;
  }

  std::vector<CaretStop> caret_stops(const TextKey& key, float size) {
    std::lock_guard lock(mutex_);
    PangoLayout* layout = build_layout(key, size, 0.0F);
    std::vector<CaretStop> stops;
    constexpr float units = 1.0F / (static_cast<float>(PANGO_SCALE) * text_scale);
    for (std::size_t offset = 0;; ++offset) {
      const bool boundary = offset == key.text.size() ||
          (static_cast<unsigned char>(key.text[offset]) & 0xC0U) != 0x80U;
      if (boundary) {
        PangoRectangle position{};
        pango_layout_index_to_pos(layout, static_cast<int>(offset), &position);
        stops.push_back({offset, static_cast<float>(position.x) * units});
      }
      if (offset == key.text.size()) break;
    }
    g_object_unref(layout);
    return stops;
  }

 private:
  PangoLayout* build_layout(const TextKey& key, float size, float max_width) const {
    PangoLayout* layout = pango_layout_new(context_);
    PangoFontDescription* description = pango_font_description_new();
    pango_font_description_set_family(description, key.family.c_str());
    pango_font_description_set_weight(description, pango_weight(key.weight));
    pango_font_description_set_absolute_size(
        description, static_cast<double>(std::max(1.0F, size) * text_scale) * PANGO_SCALE);
    pango_layout_set_font_description(layout, description);
    pango_font_description_free(description);
    pango_layout_set_text(layout, key.text.data(), static_cast<int>(key.text.size()));

    if (max_width > 0.0F) {
      PangoAttrList* attributes = pango_attr_list_new();
      PangoAttribute* hyphens = pango_attr_insert_hyphens_new(FALSE);
      hyphens->start_index = 0;
      hyphens->end_index = PANGO_ATTR_INDEX_TO_TEXT_END;
      pango_attr_list_insert(attributes, hyphens);
      pango_layout_set_attributes(layout, attributes);
      pango_attr_list_unref(attributes);
      pango_layout_set_width(
          layout, static_cast<int>(max_width * text_scale * static_cast<float>(PANGO_SCALE)));
      pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
      const int line_budget = key.max_lines > 0
          ? std::min(key.max_lines, hard_max_lines) : hard_max_lines;
      pango_layout_set_height(layout, -line_budget);
      pango_layout_set_ellipsize(layout, pango_ellipsize(key.ellipsize));
    } else {
      pango_layout_set_width(layout, -1);
      if (key.max_lines > 0) {
        pango_layout_set_height(layout, -std::min(key.max_lines, hard_max_lines));
        pango_layout_set_ellipsize(layout, pango_ellipsize(key.ellipsize));
      }
    }
    pango_layout_set_alignment(layout, pango_align(key.align));
    return layout;
  }

  TextMetrics measure(PangoLayout* layout) const {
    PangoRectangle logical{};
    pango_layout_get_extents(layout, nullptr, &logical);
    const int baseline_units = pango_layout_get_baseline(layout);
    constexpr float units = 1.0F / (static_cast<float>(PANGO_SCALE) * text_scale);
    const float ascent = static_cast<float>(baseline_units - logical.y) * units;
    const float descent = static_cast<float>(
        logical.height - (baseline_units - logical.y)) * units;
    const VerticalExtents stable = clipping_extents(layout);
    return {
        static_cast<float>(logical.width) * units,
        stable.valid ? std::min(stable.top, -ascent) : -ascent,
        stable.valid ? std::max(stable.bottom, descent) : descent,
    };
  }

  std::shared_ptr<TextBitmap> rasterize(PangoLayout* layout,
                                         const std::array<float, 4>& color,
                                         bool alpha_mask) const {
    PangoRectangle ink{};
    PangoRectangle logical{};
    pango_layout_get_extents(layout, &ink, &logical);
    const auto ceil_pixels = [](int units) {
      return (std::max(0, units) + PANGO_SCALE - 1) / PANGO_SCALE;
    };
    const int extra_left = ceil_pixels(logical.x - ink.x);
    const int extra_right = ceil_pixels(
        ink.x + ink.width - logical.x - logical.width);
    const int extra_top = ceil_pixels(logical.y - ink.y);
    const int extra_bottom = ceil_pixels(
        ink.y + ink.height - logical.y - logical.height);
    const int width = std::max(
        1, ceil_pixels(logical.width) + extra_left + extra_right);
    const int height = std::max(
        1, ceil_pixels(logical.height) + extra_top + extra_bottom);
    const int baseline = pango_layout_get_baseline(layout) / PANGO_SCALE + extra_top;
    const TextMetrics metrics = measure(layout);

    cairo_surface_t* surface = cairo_image_surface_create(
        alpha_mask ? CAIRO_FORMAT_A8 : CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
      const std::string message = cairo_status_to_string(cairo_surface_status(surface));
      cairo_surface_destroy(surface);
      throw std::runtime_error("could not create text surface: " + message);
    }
    cairo_t* cairo = cairo_create(surface);
    if (cairo_status(cairo) != CAIRO_STATUS_SUCCESS) {
      const std::string message = cairo_status_to_string(cairo_status(cairo));
      cairo_destroy(cairo);
      cairo_surface_destroy(surface);
      throw std::runtime_error("could not create text context: " + message);
    }
    cairo_set_font_options(cairo, font_options_);
    cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_OVER);
    if (alpha_mask) cairo_set_source_rgba(cairo, 0.0, 0.0, 0.0, 1.0);
    else cairo_set_source_rgb(cairo, color[0], color[1], color[2]);

    const int block_left = logical.x / PANGO_SCALE;
    PangoLayoutIter* iter = pango_layout_get_iter(layout);
    do {
      PangoRectangle line_logical{};
      pango_layout_iter_get_line_extents(iter, nullptr, &line_logical);
      const int line_left = line_logical.x / PANGO_SCALE;
      const int line_baseline = pango_layout_iter_get_baseline(iter) / PANGO_SCALE;
      cairo_move_to(cairo, line_left - block_left + extra_left,
                    line_baseline + extra_top);
      pango_cairo_show_layout_line(cairo,
                                   pango_layout_iter_get_line_readonly(iter));
    } while (pango_layout_iter_next_line(iter));
    pango_layout_iter_free(iter);
    cairo_destroy(cairo);
    cairo_surface_flush(surface);

    auto bitmap = std::make_shared<TextBitmap>();
    bitmap->width = static_cast<std::uint32_t>(width);
    bitmap->height = static_cast<std::uint32_t>(height);
    bitmap->offset_x = static_cast<float>(-extra_left) / text_scale;
    bitmap->offset_y = -metrics.top - static_cast<float>(baseline) / text_scale;
    bitmap->metrics = metrics;
    bitmap->alpha_mask = alpha_mask;
    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
    std::vector<std::uint32_t> pixels(alpha_mask ? (pixel_count + 3U) / 4U : pixel_count);
    const unsigned char* source = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const std::size_t index = static_cast<std::size_t>(y) * width + x;
        if (alpha_mask) {
          const std::uint32_t alpha = source[y * stride + x];
          pixels[index / 4U] |= alpha << ((index & 3U) * 8U);
        } else {
          const unsigned char* pixel = source + y * stride + x * 4;
          pixels[index] = static_cast<std::uint32_t>(pixel[2]) |
              static_cast<std::uint32_t>(pixel[1]) << 8U |
              static_cast<std::uint32_t>(pixel[0]) << 16U |
              static_cast<std::uint32_t>(pixel[3]) << 24U;
        }
      }
    }
    cairo_surface_destroy(surface);
    bitmap->pixels = std::make_shared<const std::vector<std::uint32_t>>(std::move(pixels));
    return bitmap;
  }

  void evict_if_needed() {
    while (lru_.size() > 1 &&
           (cache_.size() > max_cache_entries || cache_bytes_ > max_cache_bytes)) {
      const auto found = cache_.find(lru_.back());
      if (found != cache_.end()) {
        cache_bytes_ -= found->second.bytes;
        cache_.erase(found);
      }
      lru_.pop_back();
    }
  }

  PangoFontMap* font_map_ = nullptr;
  PangoContext* context_ = nullptr;
  cairo_font_options_t* font_options_ = cairo_font_options_create();
  std::mutex mutex_;
  std::unordered_map<TextKey, CacheEntry, TextKeyHash> cache_;
  std::list<TextKey> lru_;
  std::size_t cache_bytes_ = 0;
};

TextRenderer& text_renderer() {
  static TextRenderer renderer;
  return renderer;
}

}  // namespace

std::vector<CaretStop> caret_stops(std::string_view text, std::string_view font, float size,
                                   int weight) {
  if (!std::isfinite(size) || size <= 0.0F)
    throw std::invalid_argument("text size must be positive and finite");
  TextKey key;
  key.text.assign(text);
  key.family.assign(font.empty() ? "sans-serif" : font);
  key.size_bits = std::bit_cast<std::uint32_t>(size);
  key.weight = weight;
  return text_renderer().caret_stops(key, size);
}

std::pair<float, float> add_text(Scene& scene, std::string_view text,
                                 std::string_view font, float size, int weight,
                                 const Rectangle& style,
                                 const TextOptions& options) {
  if (!std::isfinite(size) || size <= 0.0F)
    throw std::invalid_argument("text size must be positive and finite");
  if (!std::isfinite(options.max_width) || options.max_width < 0.0F)
    throw std::invalid_argument("text max_width must be finite and non-negative");
  if (options.max_lines < 0)
    throw std::invalid_argument("text max_lines cannot be negative");
  if (text.empty()) return {0.0F, 0.0F};

  const bool alpha_mask = !contains_color_glyph(text);
  TextKey key;
  key.text.assign(text);
  key.family.assign(font.empty() ? "sans-serif" : font);
  key.size_bits = std::bit_cast<std::uint32_t>(size);
  key.max_width_bits = std::bit_cast<std::uint32_t>(options.max_width);
  key.color_rgb = alpha_mask ? 0U : pack_rgb(style.color);
  key.weight = weight;
  key.max_lines = options.max_lines;
  key.align = parse_align(options.align);
  key.ellipsize = parse_ellipsize(options.ellipsize);

  const auto bitmap = text_renderer().render(
      key, size, options.max_width, style.color);
  Rectangle carrier = style;
  carrier.x += bitmap->offset_x;
  carrier.y += bitmap->offset_y;
  carrier.width = static_cast<float>(bitmap->width) / text_scale;
  carrier.height = static_cast<float>(bitmap->height) / text_scale;
  // Ink may extend left or above the logical origin. Clip only the raster;
  // keep Pango's logical metrics intact for parent layout and hit testing.
  const auto clipped_pixels = [](float origin, std::uint32_t extent) {
    return std::min(extent, static_cast<std::uint32_t>(
        std::max(0.0F, -std::round(origin))));
  };
  const std::uint32_t clip_x = clipped_pixels(carrier.x, bitmap->width);
  const std::uint32_t clip_y = clipped_pixels(carrier.y, bitmap->height);
  if (clip_x == bitmap->width || clip_y == bitmap->height)
    return {bitmap->metrics.width, bitmap->metrics.bottom - bitmap->metrics.top};
  carrier.x = std::max(0.0F, carrier.x + static_cast<float>(clip_x));
  carrier.y = std::max(0.0F, carrier.y + static_cast<float>(clip_y));
  const std::uint32_t visible_width = bitmap->width - clip_x;
  const std::uint32_t visible_height = bitmap->height - clip_y;
  carrier.width = static_cast<float>(visible_width) / text_scale;
  carrier.height = static_cast<float>(visible_height) / text_scale;
  carrier.visible = false;
  carrier.color = bitmap->alpha_mask
      ? style.color
      : std::array<float, 4>{1.0F, 1.0F, 1.0F, style.color[3]};
  const std::size_t carrier_index = scene.rectangles.size();
  scene.add(carrier);
  if (clip_x == 0 && clip_y == 0) {
    Raster raster;
    raster.carrier = carrier_index;
    raster.width = bitmap->width;
    raster.height = bitmap->height;
    raster.rgba = bitmap->pixels;
    raster.use_tint = raster.alpha_mask = bitmap->alpha_mask;
    raster.pixel_snap = true;
    scene.rasters.push_back(std::move(raster));
  } else {
    const std::size_t count = static_cast<std::size_t>(visible_width) * visible_height;
    std::vector<std::uint32_t> pixels(bitmap->alpha_mask ? (count + 3U) / 4U : count);
    for (std::uint32_t y = 0; y < visible_height; ++y) {
      for (std::uint32_t x = 0; x < visible_width; ++x) {
        const std::size_t src = static_cast<std::size_t>(y + clip_y) * bitmap->width + x + clip_x;
        const std::size_t dst = static_cast<std::size_t>(y) * visible_width + x;
        if (bitmap->alpha_mask) {
          const auto alpha = ((*bitmap->pixels)[src / 4U] >> ((src & 3U) * 8U)) & 255U;
          pixels[dst / 4U] |= alpha << ((dst & 3U) * 8U);
        } else pixels[dst] = (*bitmap->pixels)[src];
      }
    }
    Raster raster;
    raster.carrier = carrier_index;
    raster.width = visible_width;
    raster.height = visible_height;
    raster.rgba = std::make_shared<const std::vector<std::uint32_t>>(std::move(pixels));
    raster.use_tint = raster.alpha_mask = bitmap->alpha_mask;
    raster.pixel_snap = true;
    scene.rasters.push_back(std::move(raster));
  }
  return {bitmap->metrics.width, bitmap->metrics.bottom - bitmap->metrics.top};
}

}  // namespace vw
