#include "vw/scene.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vw {

void Scene::add(Rectangle rectangle) {
  if (!std::isfinite(rectangle.x) || !std::isfinite(rectangle.y) ||
      !std::isfinite(rectangle.width) || !std::isfinite(rectangle.height) ||
      !std::isfinite(rectangle.radius) || !std::isfinite(rectangle.border_width) ||
      rectangle.width <= 0.0F || rectangle.height <= 0.0F ||
      rectangle.radius < 0.0F || rectangle.border_width < 0.0F) {
    // Positions may be negative: the shaders clip to the surface, so
    // widgets can slide in from past an edge.
    throw std::invalid_argument("rectangle geometry must be finite, with positive size");
  }
  rectangle.radius = std::min(rectangle.radius,
                               std::min(rectangle.width, rectangle.height) * 0.5F);
  for (float& radius : rectangle.corner_radii) {
    if (!std::isfinite(radius) || radius < -1.0F)
      throw std::invalid_argument("corner radii must be finite and non-negative or inherit");
    if (radius >= 0.0F)
      radius = std::min(radius, std::min(rectangle.width, rectangle.height) * 0.5F);
  }
  rectangle.border_width = std::min(
      rectangle.border_width, std::min(rectangle.width, rectangle.height) * 0.5F);
  width = std::max(width, static_cast<std::uint32_t>(
                              std::ceil(rectangle.x + rectangle.width)));
  height = std::max(height, static_cast<std::uint32_t>(
                                std::ceil(rectangle.y + rectangle.height)));
  rectangles.push_back(rectangle);
}

}  // namespace vw
