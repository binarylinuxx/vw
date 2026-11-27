#pragma once

// Shared pieces of vw's image decoders (image_decode.cpp, webp_decode.cpp).

#include "image_decode.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace vw::detail {

[[noreturn]] void fail(const char* reason);

// An all-transparent image, after checking the size is sane.
DecodedRaster blank_raster(std::uint64_t width, std::uint64_t height);

bool is_webp(std::span<const std::uint8_t> bytes);
// zlib (RFC 1950) and gzip (RFC 1952) streams, refusing more than `limit` bytes.
std::vector<std::uint8_t> zlib_decompress(std::span<const std::uint8_t> data, std::size_t limit);
std::vector<std::uint8_t> gzip_decompress(std::span<const std::uint8_t> data, std::size_t limit);
// A CSS colour (names, #hex, rgb[a](), hsl[a](), transparent) as RGBA 0..1.
std::optional<std::array<double, 4>> parse_css_color(std::string_view text);
// A PNG of RGBA pixels: 8-bit from rgba, or 16-bit from rgba16 (the other
// pointer null), in the packings DecodedRaster uses.
std::vector<std::uint8_t> encode_png(std::uint32_t width, std::uint32_t height, const std::uint32_t* rgba,
                                     const std::uint64_t* rgba16);
// Drops 16-bit pixels to 8 bits when that loses nothing.
void narrow_if_exact(DecodedRaster& image);
DecodedRaster decode_webp(std::span<const std::uint8_t> bytes);

constexpr std::uint64_t pack_rgba16(std::uint64_t r, std::uint64_t g, std::uint64_t b, std::uint64_t a = 65535) {
  return (r & 65535U) | (g & 65535U) << 16U | (b & 65535U) << 32U | (a & 65535U) << 48U;
}

constexpr std::uint32_t pack_rgba(unsigned r, unsigned g, unsigned b, unsigned a = 255) {
  return (r & 255U) | (g & 255U) << 8U | (b & 255U) << 16U | (a & 255U) << 24U;
}

// Bounds-checked reading of a byte buffer; running out throws.
class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}
  std::size_t pos() const { return pos_; }
  std::size_t left() const { return data_.size() - pos_; }
  void seek(std::size_t pos) {
    if (pos > data_.size()) fail("truncated file");
    pos_ = pos;
  }
  void skip(std::size_t n) { (void)take(n); }
  const std::uint8_t* take(std::size_t n) {
    if (n > left()) fail("truncated file");
    const std::uint8_t* p = data_.data() + pos_;
    pos_ += n;
    return p;
  }
  std::uint8_t u8() { return *take(1); }
  std::uint16_t le16() {
    const std::uint8_t* p = take(2);
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
  }
  std::uint16_t be16() {
    const std::uint8_t* p = take(2);
    return static_cast<std::uint16_t>(p[0] << 8 | p[1]);
  }
  std::uint32_t le24() {
    const std::uint8_t* p = take(3);
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8U |
           static_cast<std::uint32_t>(p[2]) << 16U;
  }
  std::uint32_t le32() {
    const std::uint8_t* p = take(4);
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8U |
           static_cast<std::uint32_t>(p[2]) << 16U | static_cast<std::uint32_t>(p[3]) << 24U;
  }
  std::uint32_t be32() {
    const std::uint8_t* p = take(4);
    return static_cast<std::uint32_t>(p[0]) << 24U | static_cast<std::uint32_t>(p[1]) << 16U |
           static_cast<std::uint32_t>(p[2]) << 8U | static_cast<std::uint32_t>(p[3]);
  }

 private:
  std::span<const std::uint8_t> data_;
  std::size_t pos_ = 0;
};

}  // namespace vw::detail
