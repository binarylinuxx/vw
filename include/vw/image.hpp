#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>

#include "vw/scene.hpp"

namespace vw {

// Intrinsic dimensions in pixels; nullopt for unreadable or invalid images.
std::optional<std::pair<float, float>> image_size(const std::filesystem::path& path);

std::pair<float, float> add_image(Scene& scene, const std::filesystem::path& path,
                                  float width, float height, const Rectangle& style,
                                  ImageFit fit = ImageFit::stretch);
std::pair<float, float> add_image(Scene& scene, const Bitmap& bitmap,
                                  float width, float height, const Rectangle& style,
                                  ImageFit fit = ImageFit::stretch);

// How add_image gets its pixels.
struct ImageLoad {
  // Decode on a worker thread when the file is not decoded yet: the image
  // takes its box (width x height, or nothing without one) and draws
  // nothing until image_event_fd() says it is ready.
  bool async = false;
  // The drawn size changes often (animated cards, carousels). Instead of
  // resampling on the CPU for every size, the GPU crops and scales a cached
  // mipmap level that covers the size: every frame reuses the same pixels.
  // Slightly softer than the exact resample static images get.
  bool animated = false;
};

// add_image for a file, with the loading choices above. False (nothing
// added) while an async decode is pending.
bool add_image(Scene& scene, const std::filesystem::path& path, float width, float height,
               const Rectangle& style, ImageFit fit, const ImageLoad& load,
               std::pair<float, float>* size = nullptr);

// Decodes an image file (any format vw reads, SVG included) into memory,
// scaled down to fit max_width x max_height with its aspect ratio kept; 0
// leaves that side unbounded. Sources with more than eight bits a channel
// come back in Bitmap::rgba16. Null when the file cannot be read; reason,
// when given, says why.
std::optional<Bitmap> load_image(const std::filesystem::path& path, std::uint32_t max_width = 0,
                                 std::uint32_t max_height = 0, std::string* reason = nullptr);
// Writes a bitmap as PNG: 16 bits a channel when it carries rgba16,
// otherwise 8. Written to a temporary file and renamed, so readers never see
// half a file. False on failure.
bool save_png(const Bitmap& bitmap, const std::filesystem::path& path);

// Starts decoding a file on the worker thread, so drawing it later does not
// block the build. Already decoded (and unchanged) files are skipped.
void preload_image(const std::filesystem::path& path);
// Whether a file is decoded and cached, so drawing it costs no decode.
bool image_ready(const std::filesystem::path& path);
// Readable when an async decode finished; vw::App watches it.
int image_event_fd();
bool drain_image_events();

}  // namespace vw
