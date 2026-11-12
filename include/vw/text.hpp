#pragma once

#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

#include "vw/scene.hpp"

namespace vw {

struct TextOptions {
  float max_width = 0.0F;
  int max_lines = 0;
  std::string_view align = "start";
  std::string_view ellipsize = "end";
};

struct CaretStop {
  std::size_t offset = 0;  // byte offset of a UTF-8 character boundary
  float x = 0.0F;          // logical pixels from the text origin
};

// Caret positions at every character boundary of one unwrapped line,
// including the end of the text, laid out exactly as add_text draws it.
std::vector<CaretStop> caret_stops(std::string_view text, std::string_view font, float size,
                                   int weight);

std::pair<float, float> add_text(Scene& scene, std::string_view text,
                                 std::string_view font, float size, int weight,
                                 const Rectangle& style,
                                 const TextOptions& options = {});

}  // namespace vw
