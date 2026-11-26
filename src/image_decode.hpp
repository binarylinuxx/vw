#pragma once

// vw's own image decoders: PNG, JPEG (baseline and progressive, 8 and 12
// bit), WebP (lossy, lossless, alpha, first animation frame), GIF (first
// frame), BMP, TGA, PSD (composite), Radiance HDR, binary PNM, and SVG drawn
// with cairo.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>
#include <span>
#include <vector>

namespace vw::detail {

struct DecodedRaster {
  int width = 0;
  int height = 0;
  // Eight bits a channel: r | g << 8 | b << 16 | a << 24, unpremultiplied.
  std::vector<std::uint32_t> rgba;
  // Sixteen bits a channel, for sources with more than eight:
  // r | g << 16 | b << 32 | a << 48. When set, rgba is empty.
  std::vector<std::uint64_t> rgba16;

  bool deep() const { return !rgba16.empty(); }
};

// Picks the format by its contents. Throws std::runtime_error with a short
// reason when the bytes are not an image it reads.
DecodedRaster decode_raster(std::span<const std::uint8_t> bytes);

std::optional<std::pair<double, double>> svg_intrinsic_size(std::span<const std::uint8_t> bytes);

// Renders an SVG (or gzipped SVGZ) document into width x height pixels. A
// zero size takes it from the other one and the document's aspect ratio, or
// from the document alone. Relative image references resolve against `base`.
DecodedRaster render_svg(std::span<const std::uint8_t> bytes, int width, int height,
                         const std::filesystem::path& base);

}  // namespace vw::detail
