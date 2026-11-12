#pragma once

#include <filesystem>
#include <string_view>

namespace vw {

// Resolve a serialized GIcon, themed icon name, or absolute image path.
std::filesystem::path resolve_icon(std::string_view icon, int size = 48);

}  // namespace vw
