#pragma once

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

}  // namespace vw
