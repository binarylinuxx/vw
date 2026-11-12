#pragma once

#include <filesystem>
#include <utility>

#include "vw/scene.hpp"

namespace vw {

std::pair<float, float> add_image(Scene& scene, const std::filesystem::path& path,
                                  float width, float height, const Rectangle& style,
                                  ImageFit fit = ImageFit::stretch);
std::pair<float, float> add_image(Scene& scene, const Bitmap& bitmap,
                                  float width, float height, const Rectangle& style,
                                  ImageFit fit = ImageFit::stretch);

}  // namespace vw
