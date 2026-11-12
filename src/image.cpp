#include "vw/image.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <librsvg/rsvg.h>
#include <cairo.h>
#include <glib.h>
#include <webp/decode.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace vw {
namespace {

struct DecodedImage {
  int width = 0;
  int height = 0;
  std::filesystem::file_time_type modified;
  Pixels rgba;
};

// Browsers hand out WebP artwork under any extension, so go by the contents.
bool is_webp(const std::filesystem::path& path) {
  char header[12] = {};
  std::ifstream file(path, std::ios::binary);
  return file.read(header, sizeof(header)) && std::memcmp(header, "RIFF", 4) == 0 &&
         std::memcmp(header + 8, "WEBP", 4) == 0;
}

std::shared_ptr<const DecodedImage> decode_image(const std::filesystem::path& path,
                                                  float requested_width, float requested_height) {
  static std::mutex mutex;
  static std::unordered_map<std::string, std::shared_ptr<const DecodedImage>> cache;
  const bool svg = g_ascii_strcasecmp(path.extension().c_str(), ".svg") == 0 ||
                   g_ascii_strcasecmp(path.extension().c_str(), ".svgz") == 0;
  const int target_width = requested_width > 0 ? static_cast<int>(std::ceil(requested_width)) : 0;
  const int target_height = requested_height > 0 ? static_cast<int>(std::ceil(requested_height)) : 0;
  const std::string key = path.string() + (svg ? ":" + std::to_string(target_width) +
                                                    "x" + std::to_string(target_height) : "");
  const auto modified = std::filesystem::last_write_time(path);
  std::lock_guard lock(mutex);
  if (const auto found = cache.find(key);
      found != cache.end() && found->second->modified == modified) return found->second;

  int width = 0;
  int height = 0;
  auto decoded = std::make_shared<DecodedImage>();
  std::vector<std::uint32_t> rgba;
  decoded->modified = modified;
  if (svg) {
    GError* error = nullptr;
    RsvgHandle* handle = rsvg_handle_new_from_file(path.c_str(), &error);
    if (handle == nullptr) {
      const std::string message = error == nullptr ? "unknown error" : error->message;
      if (error != nullptr) g_error_free(error);
      throw std::runtime_error("could not load SVG " + path.string() + ": " + message);
    }
    double intrinsic_width = 0.0, intrinsic_height = 0.0;
    if (!rsvg_handle_get_intrinsic_size_in_pixels(handle, &intrinsic_width, &intrinsic_height)) {
      intrinsic_width = 64;
      intrinsic_height = 64;
    }
    if (intrinsic_width <= 0 || intrinsic_height <= 0) {
      intrinsic_width = 64;
      intrinsic_height = 64;
    }
    width = target_width > 0 ? target_width :
        static_cast<int>(std::ceil(target_height > 0 ? intrinsic_width * target_height / intrinsic_height
                                                    : intrinsic_width));
    height = target_height > 0 ? target_height :
        static_cast<int>(std::ceil(target_width > 0 ? intrinsic_height * target_width / intrinsic_width
                                                   : intrinsic_height));
    if (width < 1 || height < 1 || width > 8192 || height > 8192) {
      g_object_unref(handle);
      throw std::runtime_error("SVG dimensions must be between 1 and 8192 pixels");
    }
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t* context = cairo_create(surface);
    const RsvgRectangle viewport{0.0, 0.0, static_cast<double>(width), static_cast<double>(height)};
    const bool rendered = rsvg_handle_render_document(handle, context, &viewport, &error);
    cairo_destroy(context);
    g_object_unref(handle);
    if (!rendered) {
      const std::string message = error == nullptr ? "unknown error" : error->message;
      if (error != nullptr) g_error_free(error);
      cairo_surface_destroy(surface);
      throw std::runtime_error("could not render SVG " + path.string() + ": " + message);
    }
    cairo_surface_flush(surface);
    const auto* pixels = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    rgba.resize(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
      const auto* pixel = pixels + y * stride + x * 4;
      const unsigned int alpha = pixel[3];
      const auto unpremultiply = [alpha](unsigned int value) -> std::uint32_t {
        return alpha == 0 ? 0 : std::min(255U, (value * 255U + alpha / 2U) / alpha);
      };
      rgba[static_cast<std::size_t>(y) * width + x] =
          unpremultiply(pixel[2]) | unpremultiply(pixel[1]) << 8U |
          unpremultiply(pixel[0]) << 16U | alpha << 24U;
    }
    cairo_surface_destroy(surface);
  } else if (is_webp(path)) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    using Pixels = std::unique_ptr<std::uint8_t, decltype(&WebPFree)>;
    const Pixels source(WebPDecodeRGBA(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &width, &height),
                        &WebPFree);
    if (!source) throw std::runtime_error("could not decode WebP image " + key);
    rgba.resize(static_cast<std::size_t>(width) * height);
    std::memcpy(rgba.data(), source.get(), rgba.size() * sizeof(std::uint32_t));
  } else {
    int channels = 0;
    using Pixels = std::unique_ptr<stbi_uc, decltype(&stbi_image_free)>;
    Pixels source(stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha),
                  &stbi_image_free);
    if (!source) {
      const char* reason = stbi_failure_reason();
      throw std::runtime_error("could not decode image " + key +
                               (reason == nullptr ? "" : ": " + std::string(reason)));
    }
    rgba.resize(static_cast<std::size_t>(width) * height);
    for (std::size_t index = 0; index < rgba.size(); ++index) {
      const std::size_t i = index * 4;
      rgba[index] = static_cast<std::uint32_t>(source.get()[i]) |
                             static_cast<std::uint32_t>(source.get()[i + 1]) << 8U |
                             static_cast<std::uint32_t>(source.get()[i + 2]) << 16U |
                             static_cast<std::uint32_t>(source.get()[i + 3]) << 24U;
    }
  }
  decoded->rgba = std::make_shared<const std::vector<std::uint32_t>>(std::move(rgba));
  decoded->width = width;
  decoded->height = height;
  // Huge photos (wallpapers) are only ever drawn fitted, and the fitted
  // result is cached: keeping a 100 MB decode around would only cost memory.
  constexpr std::size_t retained_pixels = 16U * 1024U * 1024U;
  if (static_cast<std::size_t>(width) * static_cast<std::size_t>(height) <= retained_pixels)
    cache[key] = decoded;
  return decoded;
}

}  // namespace

namespace {

struct Crop {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// The source region `crop`, area-averaged down to target_width x
// target_height when it is larger (never upscaled: the raster shader
// stretches). Averaging per channel is enough for photos and icons.
std::vector<std::uint32_t> resample(const DecodedImage& source, const Crop& crop, int target_width,
                                    int target_height) {
  std::vector<std::uint32_t> pixels(static_cast<std::size_t>(target_width) * target_height);
  const double step_x = static_cast<double>(crop.width) / target_width;
  const double step_y = static_cast<double>(crop.height) / target_height;
  const auto* rgba = source.rgba->data();
  for (int y = 0; y < target_height; ++y) {
    const int y0 = crop.y + static_cast<int>(y * step_y);
    const int y1 = std::max(y0 + 1, std::min(crop.y + crop.height, crop.y + static_cast<int>((y + 1) * step_y)));
    for (int x = 0; x < target_width; ++x) {
      const int x0 = crop.x + static_cast<int>(x * step_x);
      const int x1 = std::max(x0 + 1, std::min(crop.x + crop.width, crop.x + static_cast<int>((x + 1) * step_x)));
      std::uint64_t sum[4] = {0, 0, 0, 0};
      for (int sy = y0; sy < y1; ++sy)
        for (int sx = x0; sx < x1; ++sx) {
          const std::uint32_t pixel = rgba[static_cast<std::size_t>(sy) * source.width + sx];
          for (int channel = 0; channel < 4; ++channel) sum[channel] += (pixel >> (channel * 8)) & 0xffU;
        }
      const auto count = static_cast<std::uint64_t>((x1 - x0) * (y1 - y0));
      std::uint32_t result = 0;
      for (int channel = 0; channel < 4; ++channel)
        result |= static_cast<std::uint32_t>(sum[channel] / count) << (channel * 8);
      pixels[static_cast<std::size_t>(y) * target_width + x] = result;
    }
  }
  return pixels;
}

// Fitted images by file, modification time, fit and size, so a fitted
// wallpaper is decoded, cropped and scaled once rather than on every build.
struct Fitted {
  Pixels pixels;
  std::uint32_t pixel_width = 0;
  std::uint32_t pixel_height = 0;
  float width = 0;
  float height = 0;
};

std::mutex fitted_mutex;
std::vector<std::pair<std::string, Fitted>>& fitted_cache() {
  static std::vector<std::pair<std::string, Fitted>> cache;
  return cache;
}


// Crops and downscales `source` to width x height (either 0 keeps the
// natural size) the way `fit` asks.
Fitted fit_image(const DecodedImage& source_image, float width, float height, ImageFit fit) {
  const DecodedImage* source = &source_image;
  const bool sized = width > 0 && height > 0;
  std::uint32_t source_width = static_cast<std::uint32_t>(source->width);
  std::uint32_t source_height = static_cast<std::uint32_t>(source->height);
  Pixels pixels = source->rgba;
  if (sized && source->width > 0 && source->height > 0) {
    const double box = static_cast<double>(width) / height;
    const double image = static_cast<double>(source->width) / source->height;
    Crop crop{0, 0, source->width, source->height};
    if (fit == ImageFit::contain) {
      if (image > box) height = static_cast<float>(width / image);
      else width = static_cast<float>(height * image);
    } else if (fit == ImageFit::cover && std::abs(image - box) > 1e-6) {
      if (image > box) {
        crop.width = std::max(1, static_cast<int>(std::lround(source->height * box)));
        crop.x = (source->width - crop.width) / 2;
      } else {
        crop.height = std::max(1, static_cast<int>(std::lround(source->width / box)));
        crop.y = (source->height - crop.height) / 2;
      }
    }
    const int target_width = std::max(1, static_cast<int>(std::ceil(width)));
    const int target_height = std::max(1, static_cast<int>(std::ceil(height)));
    if (crop.width > target_width || crop.height > target_height) {
      const int scaled_width = std::min(crop.width, target_width);
      const int scaled_height = std::min(crop.height, target_height);
      pixels = std::make_shared<const std::vector<std::uint32_t>>(
          resample(*source, crop, scaled_width, scaled_height));
      source_width = static_cast<std::uint32_t>(scaled_width);
      source_height = static_cast<std::uint32_t>(scaled_height);
    } else if (crop.width != source->width || crop.height != source->height) {
      pixels = std::make_shared<const std::vector<std::uint32_t>>(
          resample(*source, crop, crop.width, crop.height));
      source_width = static_cast<std::uint32_t>(crop.width);
      source_height = static_cast<std::uint32_t>(crop.height);
    }
  }
  const unsigned int output_width = static_cast<unsigned int>(
      width > 0 ? width : static_cast<float>(source->width));
  const unsigned int output_height = static_cast<unsigned int>(
      height > 0 ? height : static_cast<float>(source->height));
  if (output_width == 0 || output_height == 0 || output_width > 8192 || output_height > 8192) {
    throw std::runtime_error("image dimensions must be between 1 and 8192 pixels");
  }
  const Fitted fitted{std::move(pixels), source_width, source_height,
                      static_cast<float>(output_width), static_cast<float>(output_height)};
  return fitted;
}
}  // namespace

namespace {

void push_fitted(Scene& scene, const Fitted& fitted, const Rectangle& style) {
  Rectangle carrier = style;
  carrier.width = fitted.width;
  carrier.height = fitted.height;
  carrier.visible = false;
  const std::size_t carrier_index = scene.rectangles.size();
  scene.add(carrier);
  scene.rasters.push_back(Raster{carrier_index, fitted.pixel_width, fitted.pixel_height, fitted.pixels});
}

// Fitted bitmaps by their pixels, fit and size: a capture redrawn on every
// build is downscaled once. Holding the pixels keeps the key unambiguous.
struct FittedBitmap {
  Pixels source;
  float width = 0;
  float height = 0;
  ImageFit fit = ImageFit::stretch;
  Fitted fitted;
};

std::vector<FittedBitmap>& fitted_bitmaps() {
  static std::vector<FittedBitmap> cache;
  return cache;
}

}  // namespace

std::pair<float, float> add_image(Scene& scene, const Bitmap& bitmap,
                                  float width, float height, const Rectangle& style,
                                  ImageFit fit) {
  if (bitmap.empty() || bitmap.rgba->size() != static_cast<std::size_t>(bitmap.width) * bitmap.height)
    throw std::invalid_argument("bitmap pixels do not match its size");
  std::lock_guard lock(fitted_mutex);
  auto& cache = fitted_bitmaps();
  const auto found = std::ranges::find_if(cache, [&](const FittedBitmap& entry) {
    return entry.source == bitmap.rgba && entry.width == width && entry.height == height &&
           entry.fit == fit;
  });
  if (found == cache.end()) {
    const DecodedImage source{static_cast<int>(bitmap.width), static_cast<int>(bitmap.height), {},
                              bitmap.rgba};
    if (cache.size() >= 16) cache.erase(cache.begin());
    cache.push_back({bitmap.rgba, width, height, fit, fit_image(source, width, height, fit)});
  }
  const Fitted& fitted = found == cache.end() ? cache.back().fitted : found->fitted;
  push_fitted(scene, fitted, style);
  return {fitted.width, fitted.height};
}

std::pair<float, float> add_image(Scene& scene, const std::filesystem::path& path,
                                  float width, float height, const Rectangle& style,
                                  ImageFit fit) {
  const auto push = [&](const Fitted& fitted) {
    push_fitted(scene, fitted, style);
    return std::pair{fitted.width, fitted.height};
  };
  const bool sized = width > 0 && height > 0;
  std::string key;
  if (sized) {
    key = path.string() + '\x1f' + std::to_string(static_cast<int>(fit)) + '\x1f' +
          std::to_string(width) + 'x' + std::to_string(height) + '\x1f' +
          std::to_string(std::filesystem::last_write_time(path).time_since_epoch().count());
    std::lock_guard lock(fitted_mutex);
    for (const auto& [entry_key, fitted] : fitted_cache())
      if (entry_key == key) return push(fitted);
  }

  const Fitted fitted = fit_image(*decode_image(path, width, height), width, height, fit);
  if (sized) {
    std::lock_guard lock(fitted_mutex);
    auto& cache = fitted_cache();
    if (cache.size() >= 64) cache.erase(cache.begin());
    cache.emplace_back(key, fitted);
  }
  return push(fitted);
}

}  // namespace vw
