#pragma once

#include <string_view>
#include <utility>
#include "vw/scene.hpp"

namespace vw {
std::pair<float, float> add_canvas(Scene& scene, std::string_view id,
                                  std::string_view code, const Rectangle& style,
                                  bool animate);
}
