// vw's raster decoders. Each one turns a whole file in memory into RGBA8.
#include "image_decode.hpp"
#include "image_decode_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vw::detail {

void fail(const char* reason) { throw std::runtime_error(reason); }

DecodedRaster blank_raster(std::uint64_t width, std::uint64_t height) {
  if (width == 0 || height == 0) fail("image has no pixels");
  if (width > (1U << 24) || height > (1U << 24) || width * height > (1ULL << 28))
    fail("image is too large");
  DecodedRaster image;
  image.width = static_cast<int>(width);
  image.height = static_cast<int>(height);
  image.rgba.assign(static_cast<std::size_t>(width * height), 0);
  return image;
}

// ---------------------------------------------------------------------------
// DEFLATE (RFC 1951) and the zlib wrapper (RFC 1950)

namespace {

class LsbBits {
 public:
  explicit LsbBits(std::span<const std::uint8_t> data)
      : p_(data.data()), end_(data.data() + data.size()) {}
  std::uint32_t peek(int n) {
    refill();
    return static_cast<std::uint32_t>(buffer_ & ((1ULL << n) - 1U));
  }
  void consume(int n) {
    buffer_ >>= n;
    count_ -= n;
    if (count_ < padded_ * 8) fail("compressed data is truncated");
  }
  std::uint32_t bits(int n) {
    if (n == 0) return 0;
    const std::uint32_t value = peek(n);
    consume(n);
    return value;
  }
  void align() { consume(count_ % 8); }

 private:
  void refill() {
    while (count_ <= 56) {
      std::uint64_t byte = 0;
      if (p_ < end_) byte = *p_++;
      else ++padded_;
      buffer_ |= byte << count_;
      count_ += 8;
    }
  }
  const std::uint8_t* p_;
  const std::uint8_t* end_;
  std::uint64_t buffer_ = 0;
  int count_ = 0;
  int padded_ = 0;
};

class InflateHuffman {
 public:
  static constexpr int fast_bits = 10;
  void build(const std::uint8_t* lengths, int n) {
    std::array<int, 16> count{};
    for (int i = 0; i < n; ++i) ++count[lengths[i]];
    count[0] = 0;
    int left = 1;
    for (int length = 1; length < 16; ++length) {
      left <<= 1;
      left -= count[length];
      if (left < 0) fail("invalid Huffman code");
    }
    std::array<int, 16> offset{};
    for (int length = 1; length < 15; ++length) offset[length + 1] = offset[length] + count[length];
    for (int i = 0; i < n; ++i)
      if (lengths[i] != 0) symbol_[static_cast<std::size_t>(offset[lengths[i]]++)] = static_cast<std::uint16_t>(i);
    count_ = count;
    fast_.fill(0);
    std::array<int, 16> next{};
    int code = 0;
    for (int length = 1; length < 16; ++length) {
      code = (code + count[length - 1]) << 1;
      next[length] = code;
    }
    for (int i = 0; i < n; ++i) {
      const int length = lengths[i];
      if (length == 0) continue;
      const int value = next[length]++;
      if (length > fast_bits) continue;
      int reversed = 0;
      for (int bit = 0; bit < length; ++bit) reversed |= ((value >> bit) & 1) << (length - 1 - bit);
      for (int r = reversed; r < (1 << fast_bits); r += 1 << length)
        fast_[static_cast<std::size_t>(r)] = static_cast<std::uint16_t>(length << 9 | i);
    }
  }
  int decode(LsbBits& in) const {
    const std::uint32_t window = in.peek(15);
    if (const std::uint16_t entry = fast_[window & ((1U << fast_bits) - 1U)]) {
      in.consume(entry >> 9);
      return entry & 511;
    }
    int code = 0, first = 0, index = 0;
    for (int length = 1; length < 16; ++length) {
      code |= static_cast<int>((window >> (length - 1)) & 1U);
      const int count = count_[static_cast<std::size_t>(length)];
      if (code - first < count) {
        in.consume(length);
        return symbol_[static_cast<std::size_t>(index + code - first)];
      }
      index += count;
      first = (first + count) << 1;
      code <<= 1;
    }
    fail("invalid Huffman code");
  }

 private:
  std::array<std::uint16_t, 1 << fast_bits> fast_{};
  std::array<int, 16> count_{};
  std::array<std::uint16_t, 320> symbol_{};
};

std::vector<std::uint8_t> inflate(std::span<const std::uint8_t> data, std::size_t limit) {
  static constexpr std::array<std::uint16_t, 29> length_base{
      3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static constexpr std::array<std::uint8_t, 29> length_extra{
      0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static constexpr std::array<std::uint16_t, 30> distance_base{
      1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
      257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static constexpr std::array<std::uint8_t, 30> distance_extra{
      0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  static constexpr std::array<std::uint8_t, 19> order{
      16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

  std::vector<std::uint8_t> out;
  out.reserve(limit);
  LsbBits in(data);
  InflateHuffman literals, distances;
  bool last = false;
  while (!last) {
    last = in.bits(1) != 0;
    const std::uint32_t type = in.bits(2);
    if (type == 0) {
      in.align();
      const std::uint32_t length = in.bits(16);
      if ((length ^ 0xFFFFU) != in.bits(16)) fail("corrupt stored block");
      if (out.size() + length > limit) fail("too much image data");
      for (std::uint32_t i = 0; i < length; ++i) out.push_back(static_cast<std::uint8_t>(in.bits(8)));
      continue;
    }
    if (type == 1) {
      std::array<std::uint8_t, 288> lengths{};
      std::fill(lengths.begin(), lengths.begin() + 144, 8);
      std::fill(lengths.begin() + 144, lengths.begin() + 256, 9);
      std::fill(lengths.begin() + 256, lengths.begin() + 280, 7);
      std::fill(lengths.begin() + 280, lengths.end(), 8);
      literals.build(lengths.data(), 288);
      std::array<std::uint8_t, 30> distance_lengths{};
      distance_lengths.fill(5);
      distances.build(distance_lengths.data(), 30);
    } else if (type == 2) {
      const int literal_count = static_cast<int>(in.bits(5)) + 257;
      const int distance_count = static_cast<int>(in.bits(5)) + 1;
      const int code_count = static_cast<int>(in.bits(4)) + 4;
      std::array<std::uint8_t, 19> code_lengths{};
      for (int i = 0; i < code_count; ++i) code_lengths[order[static_cast<std::size_t>(i)]] = static_cast<std::uint8_t>(in.bits(3));
      InflateHuffman codes;
      codes.build(code_lengths.data(), 19);
      std::array<std::uint8_t, 320> lengths{};
      int index = 0;
      while (index < literal_count + distance_count) {
        const int symbol = codes.decode(in);
        if (symbol < 16) {
          lengths[static_cast<std::size_t>(index++)] = static_cast<std::uint8_t>(symbol);
          continue;
        }
        std::uint8_t value = 0;
        int repeat = 0;
        if (symbol == 16) {
          if (index == 0) fail("corrupt code lengths");
          value = lengths[static_cast<std::size_t>(index - 1)];
          repeat = 3 + static_cast<int>(in.bits(2));
        } else if (symbol == 17) {
          repeat = 3 + static_cast<int>(in.bits(3));
        } else {
          repeat = 11 + static_cast<int>(in.bits(7));
        }
        if (index + repeat > literal_count + distance_count) fail("corrupt code lengths");
        while (repeat-- > 0) lengths[static_cast<std::size_t>(index++)] = value;
      }
      if (lengths[256] == 0) fail("missing end of block code");
      literals.build(lengths.data(), literal_count);
      distances.build(lengths.data() + literal_count, distance_count);
    } else {
      fail("invalid block type");
    }
    for (;;) {
      int symbol = literals.decode(in);
      if (symbol < 256) {
        if (out.size() >= limit) fail("too much image data");
        out.push_back(static_cast<std::uint8_t>(symbol));
        continue;
      }
      if (symbol == 256) break;
      symbol -= 257;
      if (symbol >= 29) fail("invalid length code");
      const std::size_t length = length_base[static_cast<std::size_t>(symbol)] +
                                 in.bits(length_extra[static_cast<std::size_t>(symbol)]);
      const int code = distances.decode(in);
      if (code >= 30) fail("invalid distance code");
      const std::size_t distance = distance_base[static_cast<std::size_t>(code)] +
                                   in.bits(distance_extra[static_cast<std::size_t>(code)]);
      if (distance > out.size()) fail("invalid distance");
      if (out.size() + length > limit) fail("too much image data");
      const std::size_t from = out.size() - distance;
      for (std::size_t i = 0; i < length; ++i) out.push_back(out[from + i]);
    }
  }
  return out;
}

}  // namespace

std::vector<std::uint8_t> zlib_decompress(std::span<const std::uint8_t> data, std::size_t limit) {
  if (data.size() < 2) fail("compressed data is truncated");
  const unsigned method = data[0];
  const unsigned flags = data[1];
  if ((method * 256U + flags) % 31U != 0 || (method & 15U) != 8) fail("corrupt zlib header");
  if ((flags & 0x20U) != 0) fail("zlib preset dictionary is not supported");
  return inflate(data.subspan(2), limit);
}

std::vector<std::uint8_t> gzip_decompress(std::span<const std::uint8_t> data, std::size_t limit) {
  Reader reader(data);
  if (reader.u8() != 0x1F || reader.u8() != 0x8B || reader.u8() != 8) fail("corrupt gzip header");
  const unsigned flags = reader.u8();
  reader.skip(6);
  if ((flags & 4U) != 0) reader.skip(reader.le16());
  if ((flags & 8U) != 0) while (reader.u8() != 0) {}
  if ((flags & 16U) != 0) while (reader.u8() != 0) {}
  if ((flags & 2U) != 0) reader.skip(2);
  return inflate(data.subspan(reader.pos()), limit);
}

void narrow_if_exact(DecodedRaster& image) {
  if (!image.deep()) return;
  for (const std::uint64_t pixel : image.rgba16)
    for (int shift = 0; shift < 64; shift += 16)
      if (((pixel >> shift) & 65535U) % 257U != 0) return;
  image.rgba.resize(image.rgba16.size());
  for (std::size_t i = 0; i < image.rgba16.size(); ++i) {
    const std::uint64_t p = image.rgba16[i];
    image.rgba[i] = pack_rgba(static_cast<unsigned>((p >> 8U) & 255U), static_cast<unsigned>((p >> 24U) & 255U),
                              static_cast<unsigned>((p >> 40U) & 255U), static_cast<unsigned>((p >> 56U) & 255U));
  }
  image.rgba16 = {};
}

namespace {

// ---------------------------------------------------------------------------
// PNG

constexpr std::uint32_t fourcc(const char (&name)[5]) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(name[0])) << 24U |
         static_cast<std::uint32_t>(static_cast<unsigned char>(name[1])) << 16U |
         static_cast<std::uint32_t>(static_cast<unsigned char>(name[2])) << 8U |
         static_cast<std::uint32_t>(static_cast<unsigned char>(name[3]));
}

void unfilter(int filter, std::uint8_t* row, const std::uint8_t* up, std::size_t stride, std::size_t bpp) {
  switch (filter) {
    case 0: return;
    case 1:
      for (std::size_t i = bpp; i < stride; ++i) row[i] = static_cast<std::uint8_t>(row[i] + row[i - bpp]);
      return;
    case 2:
      for (std::size_t i = 0; i < stride; ++i) row[i] = static_cast<std::uint8_t>(row[i] + up[i]);
      return;
    case 3:
      for (std::size_t i = 0; i < stride; ++i) {
        const unsigned left = i >= bpp ? row[i - bpp] : 0U;
        row[i] = static_cast<std::uint8_t>(row[i] + ((left + up[i]) >> 1U));
      }
      return;
    case 4:
      for (std::size_t i = 0; i < stride; ++i) {
        const int a = i >= bpp ? row[i - bpp] : 0;
        const int b = up[i];
        const int c = i >= bpp ? up[i - bpp] : 0;
        const int p = a + b - c;
        const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
        const int predicted = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
        row[i] = static_cast<std::uint8_t>(row[i] + predicted);
      }
      return;
    default: fail("invalid PNG filter");
  }
}

DecodedRaster decode_png(std::span<const std::uint8_t> bytes) {
  Reader reader(bytes);
  reader.skip(8);
  std::uint32_t width = 0, height = 0;
  int depth = 0, type = 0, interlace = 0;
  bool header = false;
  std::vector<std::uint8_t> compressed;
  std::array<std::uint32_t, 256> palette{};
  palette.fill(pack_rgba(0, 0, 0));
  std::size_t palette_size = 0;
  bool has_key = false;
  std::array<unsigned, 3> key{};
  while (reader.left() > 0) {
    const std::uint32_t length = reader.be32();
    const std::uint32_t kind = reader.be32();
    Reader chunk(std::span(reader.take(length), length));
    reader.skip(4);
    if (!header && kind != fourcc("IHDR")) fail("first PNG chunk is not IHDR");
    if (kind == fourcc("IHDR")) {
      if (header) fail("multiple PNG headers");
      width = chunk.be32();
      height = chunk.be32();
      depth = chunk.u8();
      type = chunk.u8();
      const int compression = chunk.u8();
      const int filter = chunk.u8();
      interlace = chunk.u8();
      if (compression != 0 || filter != 0 || interlace > 1) fail("unsupported PNG header");
      const bool valid = (type == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
                         (type == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)) ||
                         ((type == 2 || type == 4 || type == 6) && (depth == 8 || depth == 16));
      if (!valid) fail("unsupported PNG color type or depth");
      header = true;
    } else if (kind == fourcc("PLTE")) {
      if (length % 3 != 0 || length / 3 > 256) fail("invalid PNG palette");
      palette_size = length / 3;
      for (std::size_t i = 0; i < palette_size; ++i) {
        const unsigned r = chunk.u8(), g = chunk.u8(), b = chunk.u8();
        palette[i] = pack_rgba(r, g, b);
      }
    } else if (kind == fourcc("tRNS")) {
      if (type == 3) {
        for (std::size_t i = 0; i < std::min<std::size_t>(length, 256); ++i)
          palette[i] = (palette[i] & 0xFFFFFFU) | static_cast<std::uint32_t>(chunk.u8()) << 24U;
      } else if (type == 0 && length >= 2) {
        key[0] = chunk.be16();
        has_key = true;
      } else if (type == 2 && length >= 6) {
        for (unsigned& value : key) value = chunk.be16();
        has_key = true;
      }
    } else if (kind == fourcc("IDAT")) {
      const auto* data = bytes.data() + reader.pos() - 4 - length;
      compressed.insert(compressed.end(), data, data + length);
    } else if (kind == fourcc("IEND")) {
      break;
    } else if ((kind & 0x20000000U) == 0) {
      fail("unsupported critical PNG chunk");
    }
  }
  if (!header) fail("missing PNG header");
  if (compressed.empty()) fail("missing PNG image data");
  if (type == 3 && palette_size == 0) fail("missing PNG palette");

  DecodedRaster image = blank_raster(width, height);
  const int channels = type == 0 || type == 3 ? 1 : type == 2 ? 3 : type == 4 ? 2 : 4;
  const std::size_t bits_per_pixel = static_cast<std::size_t>(channels * depth);
  const std::size_t bpp = std::max<std::size_t>(1, bits_per_pixel / 8);
  struct Pass { std::uint32_t x, y, dx, dy; };
  static constexpr std::array<Pass, 7> adam7{{
      {0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}}};
  static constexpr std::array<Pass, 1> progressive{{{0, 0, 1, 1}}};
  const std::span<const Pass> passes = interlace != 0 ? std::span<const Pass>(adam7) : std::span<const Pass>(progressive);
  const auto pass_size = [&](const Pass& pass) {
    return std::pair<std::uint32_t, std::uint32_t>{
        width > pass.x ? (width - pass.x + pass.dx - 1) / pass.dx : 0,
        height > pass.y ? (height - pass.y + pass.dy - 1) / pass.dy : 0};
  };
  std::size_t expected = 0;
  for (const Pass& pass : passes) {
    const auto [pw, ph] = pass_size(pass);
    if (pw != 0 && ph != 0) expected += ph * (1 + (pw * bits_per_pixel + 7) / 8);
  }
  std::vector<std::uint8_t> raw = zlib_decompress(compressed, expected);
  if (raw.size() < expected) fail("not enough PNG image data");

  const auto sample = [depth](const std::uint8_t* row, std::size_t index) -> unsigned {
    if (depth == 8) return row[index];
    if (depth == 16) return static_cast<unsigned>(row[2 * index]) << 8U | row[2 * index + 1];
    const std::size_t bit = index * static_cast<std::size_t>(depth);
    const int shift = 8 - depth - static_cast<int>(bit & 7U);
    return (row[bit >> 3U] >> shift) & ((1U << depth) - 1U);
  };
  const auto to8 = [depth](unsigned value) -> unsigned {
    if (depth == 8) return value;
    return value * (255U / ((1U << depth) - 1U));
  };
  const bool deep = depth == 16;
  if (deep) {
    image.rgba16.assign(image.rgba.size(), 0);
    image.rgba = {};
  }
  std::vector<std::uint8_t> zeros;
  std::size_t offset = 0;
  for (const Pass& pass : passes) {
    const auto [pw, ph] = pass_size(pass);
    if (pw == 0 || ph == 0) continue;
    const std::size_t stride = (pw * bits_per_pixel + 7) / 8;
    zeros.assign(stride, 0);
    const std::uint8_t* up = zeros.data();
    for (std::uint32_t y = 0; y < ph; ++y) {
      const int filter = raw[offset];
      std::uint8_t* row = raw.data() + offset + 1;
      offset += 1 + stride;
      unfilter(filter, row, up, stride, bpp);
      up = row;
      const std::size_t line = static_cast<std::size_t>(pass.y + y * pass.dy) * width;
      for (std::uint32_t x = 0; x < pw; ++x) {
        const std::size_t base = static_cast<std::size_t>(x) * static_cast<std::size_t>(channels);
        const std::size_t at = line + pass.x + x * pass.dx;
        if (type == 3) {
          image.rgba[at] = palette[sample(row, base) & 255U];
          continue;
        }
        std::array<unsigned, 4> c{};
        bool keyed = false;
        if (type == 0 || type == 4) {
          c[0] = c[1] = c[2] = sample(row, base);
          c[3] = type == 4 ? sample(row, base + 1) : 0;
          keyed = has_key && c[0] == key[0];
        } else {
          c[0] = sample(row, base);
          c[1] = sample(row, base + 1);
          c[2] = sample(row, base + 2);
          c[3] = type == 6 ? sample(row, base + 3) : 0;
          keyed = has_key && c[0] == key[0] && c[1] == key[1] && c[2] == key[2];
        }
        if (type == 0 || type == 2) c[3] = keyed ? 0U : deep ? 65535U : 255U;
        else if (!deep) c[3] = to8(c[3]);
        if (deep) {
          image.rgba16[at] = pack_rgba16(c[0], c[1], c[2], c[3]);
        } else {
          image.rgba[at] = pack_rgba(to8(c[0]), to8(c[1]), to8(c[2]), c[3]);
        }
      }
    }
  }
  return image;
}

// ---------------------------------------------------------------------------
// JPEG (ITU T.81): baseline and progressive Huffman, 8-bit samples

constexpr std::array<std::uint8_t, 64> zigzag{
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22,
    15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct JpegHuffman {
  JpegHuffman() { max_code.fill(-1); }
  void build(const std::uint8_t* counts, const std::uint8_t* symbols, int total) {
    std::copy(symbols, symbols + total, values.begin());
    fast.fill(0);
    max_code.fill(-1);
    int code = 0, k = 0;
    for (int length = 1; length <= 16; ++length) {
      value_offset[static_cast<std::size_t>(length)] = k;
      min_code[static_cast<std::size_t>(length)] = code;
      const int n = counts[length - 1];
      for (int i = 0; i < n; ++i, ++code, ++k) {
        if (length > 9) continue;
        const int base = code << (9 - length);
        for (int j = 0; j < 1 << (9 - length); ++j)
          fast[static_cast<std::size_t>(base + j)] = static_cast<std::uint16_t>(length << 8 | values[static_cast<std::size_t>(k)]);
      }
      if (n != 0) max_code[static_cast<std::size_t>(length)] = code - 1;
      if (code > (1 << length)) fail("invalid JPEG Huffman table");
      code <<= 1;
    }
  }
  std::array<std::uint16_t, 512> fast{};
  std::array<int, 17> max_code{};
  std::array<int, 17> min_code{};
  std::array<int, 17> value_offset{};
  std::array<std::uint8_t, 256> values{};
};

class JpegBits {
 public:
  JpegBits(std::span<const std::uint8_t> data, std::size_t pos) : data_(data), pos_(pos) {}
  unsigned bits(int n) {
    if (n == 0) return 0;
    fill();
    const unsigned value = buffer_ >> (32 - n);
    buffer_ <<= n;
    count_ -= n;
    return value;
  }
  int receive(int size) {
    if (size == 0) return 0;
    if (size > 16) fail("corrupt JPEG");
    const unsigned value = bits(size);
    return value < (1U << (size - 1)) ? static_cast<int>(value) - (1 << size) + 1 : static_cast<int>(value);
  }
  int decode(const JpegHuffman& table) {
    fill();
    if (const std::uint16_t entry = table.fast[buffer_ >> 23U]) {
      const int length = entry >> 8;
      buffer_ <<= length;
      count_ -= length;
      return entry & 255;
    }
    for (int length = 10; length <= 16; ++length) {
      const int code = static_cast<int>(buffer_ >> (32 - length));
      if (code <= table.max_code[static_cast<std::size_t>(length)]) {
        buffer_ <<= length;
        count_ -= length;
        return table.values[static_cast<std::size_t>(table.value_offset[static_cast<std::size_t>(length)] + code -
                                                     table.min_code[static_cast<std::size_t>(length)])];
      }
    }
    fail("corrupt JPEG");
  }
  void restart() {
    buffer_ = 0;
    count_ = 0;
    if (marker_ == 0) find_marker();
    if (marker_ >= 0xD0 && marker_ <= 0xD7) {
      pos_ += 2;
      marker_ = 0;
    }
  }
  std::size_t end() {
    if (marker_ == 0) find_marker();
    return pos_;
  }

 private:
  void fill() {
    while (count_ <= 24) {
      unsigned byte = 0;
      if (marker_ == 0 && pos_ < data_.size()) {
        byte = data_[pos_];
        if (byte == 0xFF) {
          const unsigned next = pos_ + 1 < data_.size() ? data_[pos_ + 1] : 0xD9U;
          if (next == 0) {
            pos_ += 2;
          } else {
            marker_ = next;
            byte = 0;
          }
        } else {
          ++pos_;
        }
      }
      buffer_ |= byte << (24 - count_);
      count_ += 8;
    }
  }
  void find_marker() {
    while (pos_ + 1 < data_.size()) {
      if (data_[pos_] == 0xFF && data_[pos_ + 1] != 0 && data_[pos_ + 1] != 0xFF) {
        marker_ = data_[pos_ + 1];
        return;
      }
      ++pos_;
    }
    pos_ = data_.size();
  }
  std::span<const std::uint8_t> data_;
  std::size_t pos_;
  std::uint32_t buffer_ = 0;
  int count_ = 0;
  unsigned marker_ = 0;
};

struct JpegComponent {
  int id = 0, h = 1, v = 1, quant = 0;
  int dc_table = 0, ac_table = 0;
  int pred = 0;
  int blocks_wide = 0, blocks_high = 0;
  std::vector<std::int16_t> coefficients;
  std::vector<std::uint16_t> samples;
};

const std::array<float, 64>& idct_matrix() {
  static const std::array<float, 64> matrix = [] {
    std::array<float, 64> m{};
    for (int u = 0; u < 8; ++u)
      for (int x = 0; x < 8; ++x)
        m[static_cast<std::size_t>(u * 8 + x)] = static_cast<float>(
            (u == 0 ? std::sqrt(0.5) : 1.0) * std::cos((2 * x + 1) * u * M_PI / 16.0) / 2.0);
    return m;
  }();
  return matrix;
}

std::uint16_t clamp_sample(float value, int maximum) {
  return static_cast<std::uint16_t>(std::clamp(static_cast<int>(std::floor(value + 0.5F)), 0, maximum));
}

void inverse_dct(const std::int16_t* in, const std::array<std::uint16_t, 64>& quant, std::uint16_t* out,
                 std::size_t stride, int precision) {
  const int maximum = (1 << precision) - 1;
  const auto center = static_cast<float>(1 << (precision - 1));
  std::array<float, 64> f{};
  bool ac = false;
  for (std::size_t i = 0; i < 64; ++i) {
    f[i] = static_cast<float>(in[i]) * static_cast<float>(quant[i]);
    ac = ac || (i != 0 && in[i] != 0);
  }
  if (!ac) {
    const std::uint16_t value = clamp_sample(f[0] / 8.0F + center, maximum);
    for (int y = 0; y < 8; ++y) std::fill_n(out + static_cast<std::size_t>(y) * stride, 8, value);
    return;
  }
  const auto& m = idct_matrix();
  std::array<float, 64> t{};
  for (int v = 0; v < 8; ++v)
    for (int x = 0; x < 8; ++x) {
      float sum = 0;
      for (int u = 0; u < 8; ++u) sum += f[static_cast<std::size_t>(v * 8 + u)] * m[static_cast<std::size_t>(u * 8 + x)];
      t[static_cast<std::size_t>(v * 8 + x)] = sum;
    }
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) {
      float sum = 0;
      for (int v = 0; v < 8; ++v) sum += m[static_cast<std::size_t>(v * 8 + y)] * t[static_cast<std::size_t>(v * 8 + x)];
      out[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x)] = clamp_sample(sum + center, maximum);
    }
}

DecodedRaster decode_jpeg(std::span<const std::uint8_t> bytes) {
  Reader reader(bytes);
  reader.skip(2);
  std::array<std::array<std::uint16_t, 64>, 4> quant{};
  std::array<JpegHuffman, 4> dc_tables, ac_tables;
  std::vector<JpegComponent> components;
  int width = 0, height = 0, hmax = 1, vmax = 1, mcus_wide = 0, mcus_high = 0;
  bool progressive = false, frame = false, jfif = false;
  int precision = 8;
  int restart_interval = 0, adobe = -1;

  while (reader.left() > 0) {
    if (reader.u8() != 0xFF) continue;
    std::uint8_t marker = 0xFF;
    while (marker == 0xFF && reader.left() > 0) marker = reader.u8();
    if (marker == 0xD9) break;
    if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01 || marker == 0xFF) continue;
    const std::uint16_t length = reader.be16();
    if (length < 2) fail("corrupt JPEG segment");
    Reader segment(std::span(reader.take(length - 2U), length - 2U));
    switch (marker) {
      case 0xDB:
        while (segment.left() > 0) {
          const unsigned info = segment.u8();
          if ((info & 15U) > 3) fail("invalid JPEG quantization table");
          auto& table = quant[info & 15U];
          for (const std::uint8_t position : zigzag) table[position] = (info >> 4U) != 0 ? segment.be16() : segment.u8();
        }
        break;
      case 0xC4:
        while (segment.left() > 0) {
          const unsigned info = segment.u8();
          if ((info & 15U) > 3 || (info >> 4U) > 1) fail("invalid JPEG Huffman table");
          const std::uint8_t* counts = segment.take(16);
          int total = 0;
          for (int i = 0; i < 16; ++i) total += counts[i];
          if (total > 256) fail("invalid JPEG Huffman table");
          const std::uint8_t* symbols = segment.take(static_cast<std::size_t>(total));
          ((info >> 4U) != 0 ? ac_tables : dc_tables)[info & 15U].build(counts, symbols, total);
        }
        break;
      case 0xC0: case 0xC1: case 0xC2: {
        if (frame) fail("multiple JPEG frames");
        progressive = marker == 0xC2;
        precision = segment.u8();
        if (precision != 8 && precision != 12) fail("unsupported JPEG sample precision");
        height = segment.be16();
        width = segment.be16();
        if (height == 0) fail("JPEG height in DNL is not supported");
        (void)blank_raster(static_cast<std::uint64_t>(width), 1);
        if (static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > (1ULL << 28))
          fail("image is too large");
        const int count = segment.u8();
        if (count != 1 && count != 3 && count != 4) fail("unsupported JPEG component count");
        components.resize(static_cast<std::size_t>(count));
        for (JpegComponent& component : components) {
          component.id = segment.u8();
          const unsigned sampling = segment.u8();
          component.h = static_cast<int>(sampling >> 4U);
          component.v = static_cast<int>(sampling & 15U);
          component.quant = segment.u8();
          if (component.h < 1 || component.h > 4 || component.v < 1 || component.v > 4 || component.quant > 3)
            fail("invalid JPEG component");
          hmax = std::max(hmax, component.h);
          vmax = std::max(vmax, component.v);
        }
        mcus_wide = (width + 8 * hmax - 1) / (8 * hmax);
        mcus_high = (height + 8 * vmax - 1) / (8 * vmax);
        for (JpegComponent& component : components) {
          component.blocks_wide = mcus_wide * component.h;
          component.blocks_high = mcus_high * component.v;
          component.coefficients.assign(
              static_cast<std::size_t>(component.blocks_wide) * static_cast<std::size_t>(component.blocks_high) * 64U, 0);
        }
        frame = true;
        break;
      }
      case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB:
      case 0xCD: case 0xCE: case 0xCF:
        fail("unsupported JPEG coding process");
      case 0xDD:
        restart_interval = segment.be16();
        break;
      case 0xE0:
        if (segment.left() >= 5 && std::memcmp(segment.take(5), "JFIF\0", 5) == 0) jfif = true;
        break;
      case 0xEE:
        if (segment.left() >= 12) {
          const std::uint8_t* data = segment.take(12);
          if (std::memcmp(data, "Adobe", 5) == 0) adobe = data[11];
        }
        break;
      case 0xDA: {
        if (!frame) fail("JPEG scan before frame");
        const int count = segment.u8();
        if (count < 1 || count > 4) fail("invalid JPEG scan");
        std::vector<JpegComponent*> scan;
        for (int i = 0; i < count; ++i) {
          const int id = segment.u8();
          const unsigned tables = segment.u8();
          const auto found = std::ranges::find_if(components, [id](const JpegComponent& c) { return c.id == id; });
          if (found == components.end() || (tables >> 4U) > 3 || (tables & 15U) > 3) fail("invalid JPEG scan");
          found->dc_table = static_cast<int>(tables >> 4U);
          found->ac_table = static_cast<int>(tables & 15U);
          scan.push_back(&*found);
        }
        const int start = segment.u8();
        const int stop = segment.u8();
        const unsigned approximation = segment.u8();
        const int high = static_cast<int>(approximation >> 4U);
        const int low = static_cast<int>(approximation & 15U);
        if (progressive && (start > stop || stop > 63 || (start == 0 && stop != 0) || (start > 0 && count != 1) ||
                            low > 13))
          fail("invalid progressive JPEG scan");

        JpegBits bits(bytes, reader.pos());
        int eob_run = 0;
        for (JpegComponent* component : scan) component->pred = 0;
        const auto decode_block = [&](JpegComponent& c, std::int16_t* coef) {
          if (!progressive) {
            c.pred += bits.receive(bits.decode(dc_tables[static_cast<std::size_t>(c.dc_table)]));
            coef[0] = static_cast<std::int16_t>(c.pred);
            const JpegHuffman& ac = ac_tables[static_cast<std::size_t>(c.ac_table)];
            for (int k = 1; k < 64;) {
              const int rs = bits.decode(ac);
              const int run = rs >> 4, size = rs & 15;
              if (size == 0) {
                if (run != 15) break;
                k += 16;
                continue;
              }
              k += run;
              if (k > 63) fail("corrupt JPEG");
              coef[zigzag[static_cast<std::size_t>(k++)]] = static_cast<std::int16_t>(bits.receive(size));
            }
            return;
          }
          if (start == 0) {
            if (high == 0) {
              c.pred += bits.receive(bits.decode(dc_tables[static_cast<std::size_t>(c.dc_table)]));
              coef[0] = static_cast<std::int16_t>(c.pred * (1 << low));
            } else if (bits.bits(1) != 0) {
              coef[0] = static_cast<std::int16_t>(coef[0] | (1 << low));
            }
            return;
          }
          const JpegHuffman& ac = ac_tables[static_cast<std::size_t>(c.ac_table)];
          if (high == 0) {
            if (eob_run > 0) {
              --eob_run;
              return;
            }
            for (int k = start; k <= stop;) {
              const int rs = bits.decode(ac);
              const int run = rs >> 4, size = rs & 15;
              if (size == 0) {
                if (run < 15) {
                  eob_run = (1 << run) - 1 + static_cast<int>(bits.bits(run));
                  break;
                }
                k += 16;
                continue;
              }
              k += run;
              if (k > 63) fail("corrupt JPEG");
              coef[zigzag[static_cast<std::size_t>(k++)]] = static_cast<std::int16_t>(bits.receive(size) * (1 << low));
            }
            return;
          }
          const int plus = 1 << low, minus = -plus;
          const auto refine = [&](std::int16_t& value) {
            if (bits.bits(1) != 0 && (value & plus) == 0)
              value = static_cast<std::int16_t>(value + (value >= 0 ? plus : minus));
          };
          int k = start;
          if (eob_run <= 0) {
            while (k <= stop) {
              const int rs = bits.decode(ac);
              int run = rs >> 4;
              int value = 0;
              if ((rs & 15) == 0) {
                if (run < 15) {
                  eob_run = (1 << run) + static_cast<int>(bits.bits(run));
                  break;
                }
              } else {
                value = bits.bits(1) != 0 ? plus : minus;
              }
              while (k <= stop) {
                std::int16_t& current = coef[zigzag[static_cast<std::size_t>(k++)]];
                if (current != 0) {
                  refine(current);
                } else {
                  if (run == 0) {
                    if (value != 0) current = static_cast<std::int16_t>(value);
                    break;
                  }
                  --run;
                }
              }
            }
          }
          if (eob_run > 0) {
            for (; k <= stop; ++k) {
              std::int16_t& current = coef[zigzag[static_cast<std::size_t>(k)]];
              if (current != 0) refine(current);
            }
            --eob_run;
          }
        };
        const auto block = [](JpegComponent& c, int bx, int by) {
          return c.coefficients.data() +
                 (static_cast<std::size_t>(by) * static_cast<std::size_t>(c.blocks_wide) + static_cast<std::size_t>(bx)) * 64U;
        };
        int counter = 0;
        const auto restart = [&] {
          if (restart_interval != 0 && counter == restart_interval) {
            bits.restart();
            for (JpegComponent* component : scan) component->pred = 0;
            eob_run = 0;
            counter = 0;
          }
        };
        if (count == 1) {
          JpegComponent& c = *scan[0];
          const int samples_wide = (width * c.h + hmax - 1) / hmax;
          const int samples_high = (height * c.v + vmax - 1) / vmax;
          const int blocks_wide = (samples_wide + 7) / 8;
          const int blocks_high = (samples_high + 7) / 8;
          for (int by = 0; by < blocks_high; ++by)
            for (int bx = 0; bx < blocks_wide; ++bx) {
              restart();
              decode_block(c, block(c, bx, by));
              ++counter;
            }
        } else {
          for (int my = 0; my < mcus_high; ++my)
            for (int mx = 0; mx < mcus_wide; ++mx) {
              restart();
              for (JpegComponent* c : scan)
                for (int v = 0; v < c->v; ++v)
                  for (int h = 0; h < c->h; ++h) decode_block(*c, block(*c, mx * c->h + h, my * c->v + v));
              ++counter;
            }
        }
        reader.seek(bits.end());
        break;
      }
      default:
        break;
    }
  }
  if (!frame) fail("missing JPEG frame");

  for (JpegComponent& c : components) {
    const std::size_t stride = static_cast<std::size_t>(c.blocks_wide) * 8U;
    c.samples.assign(stride * static_cast<std::size_t>(c.blocks_high) * 8U, 0);
    for (int by = 0; by < c.blocks_high; ++by)
      for (int bx = 0; bx < c.blocks_wide; ++bx)
        inverse_dct(c.coefficients.data() + (static_cast<std::size_t>(by) * static_cast<std::size_t>(c.blocks_wide) +
                                             static_cast<std::size_t>(bx)) * 64U,
                    quant[static_cast<std::size_t>(c.quant)],
                    c.samples.data() + static_cast<std::size_t>(by) * 8U * stride + static_cast<std::size_t>(bx) * 8U,
                    stride, precision);
    c.coefficients = {};
  }

  // Subsampled planes are stretched to full size with centered bilinear
  // filtering.
  const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  const int maximum = (1 << precision) - 1;
  std::vector<std::vector<std::uint16_t>> planes(components.size());
  for (std::size_t index = 0; index < components.size(); ++index) {
    const JpegComponent& c = components[index];
    const std::size_t stride = static_cast<std::size_t>(c.blocks_wide) * 8U;
    auto& plane = planes[index];
    plane.resize(pixels);
    if (c.h == hmax && c.v == vmax) {
      for (int y = 0; y < height; ++y)
        std::copy_n(c.samples.data() + static_cast<std::size_t>(y) * stride, width,
                    plane.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width));
      continue;
    }
    const int samples_wide = (width * c.h + hmax - 1) / hmax;
    const int samples_high = (height * c.v + vmax - 1) / vmax;
    struct Tap { int a, b; float t; };
    const auto taps = [](int count, int samples, float ratio) {
      std::vector<Tap> result(static_cast<std::size_t>(count));
      for (int i = 0; i < count; ++i) {
        const float position = std::max(0.0F, (static_cast<float>(i) + 0.5F) * ratio - 0.5F);
        const int a = std::min(static_cast<int>(position), samples - 1);
        result[static_cast<std::size_t>(i)] = {a, std::min(a + 1, samples - 1), position - static_cast<float>(a)};
      }
      return result;
    };
    const auto xs = taps(width, samples_wide, static_cast<float>(c.h) / static_cast<float>(hmax));
    const auto ys = taps(height, samples_high, static_cast<float>(c.v) / static_cast<float>(vmax));
    for (int y = 0; y < height; ++y) {
      const Tap& ty = ys[static_cast<std::size_t>(y)];
      const std::uint16_t* row0 = c.samples.data() + static_cast<std::size_t>(ty.a) * stride;
      const std::uint16_t* row1 = c.samples.data() + static_cast<std::size_t>(ty.b) * stride;
      for (int x = 0; x < width; ++x) {
        const Tap& tx = xs[static_cast<std::size_t>(x)];
        const float top = row0[tx.a] + static_cast<float>(row0[tx.b] - row0[tx.a]) * tx.t;
        const float bottom = row1[tx.a] + static_cast<float>(row1[tx.b] - row1[tx.a]) * tx.t;
        plane[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] =
            clamp_sample(top + (bottom - top) * ty.t, maximum);
      }
    }
  }
  for (JpegComponent& c : components) c.samples = {};

  DecodedRaster image = blank_raster(static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height));
  const bool deep = precision > 8;
  if (deep) {
    image.rgba16.assign(image.rgba.size(), 0);
    image.rgba = {};
  }
  // Colour work in 0..1; 12-bit sources come out with 16 bits a channel.
  const float scale = 1.0F / static_cast<float>(maximum);
  const auto ycc = [](float y, float cb, float cr) {
    cb -= 0.5F;
    cr -= 0.5F;
    return std::array<float, 3>{y + 1.402F * cr, y - 0.344136F * cb - 0.714136F * cr, y + 1.772F * cb};
  };
  const bool rgb = components.size() == 3 &&
                   ((components[0].id == 'R' && components[1].id == 'G' && components[2].id == 'B') ||
                    (adobe == 0 && !jfif));
  for (std::size_t i = 0; i < pixels; ++i) {
    std::array<float, 3> c{};
    const auto sample = [&](std::size_t plane) { return static_cast<float>(planes[plane][i]) * scale; };
    if (components.size() == 1) {
      c[0] = c[1] = c[2] = sample(0);
    } else if (components.size() == 3) {
      c = rgb ? std::array<float, 3>{sample(0), sample(1), sample(2)} : ycc(sample(0), sample(1), sample(2));
    } else {
      const float k = sample(3);
      if (adobe == 0) {
        c = {sample(0) * k, sample(1) * k, sample(2) * k};
      } else if (adobe == 2) {
        c = ycc(sample(0), sample(1), sample(2));
        for (float& value : c) value = (1.0F - std::clamp(value, 0.0F, 1.0F)) * k;
      } else {
        c = ycc(sample(0), sample(1), sample(2));
      }
    }
    if (deep) {
      const auto to16 = [](float v) { return static_cast<std::uint64_t>(std::clamp(v, 0.0F, 1.0F) * 65535.0F + 0.5F); };
      image.rgba16[i] = pack_rgba16(to16(c[0]), to16(c[1]), to16(c[2]));
    } else {
      const auto to8 = [](float v) { return static_cast<unsigned>(std::clamp(v, 0.0F, 1.0F) * 255.0F + 0.5F); };
      image.rgba[i] = pack_rgba(to8(c[0]), to8(c[1]), to8(c[2]));
    }
  }
  return image;
}

// ---------------------------------------------------------------------------
// GIF: the first frame on its logical screen

DecodedRaster decode_gif(std::span<const std::uint8_t> bytes) {
  Reader reader(bytes);
  reader.skip(6);
  const unsigned screen_width = reader.le16();
  const unsigned screen_height = reader.le16();
  const unsigned flags = reader.u8();
  reader.skip(2);
  std::array<std::uint32_t, 256> global{};
  global.fill(pack_rgba(0, 0, 0));
  if ((flags & 0x80U) != 0) {
    const unsigned count = 2U << (flags & 7U);
    for (unsigned i = 0; i < count; ++i) {
      const unsigned r = reader.u8(), g = reader.u8(), b = reader.u8();
      global[i] = pack_rgba(r, g, b);
    }
  }
  const auto skip_blocks = [&] {
    while (const unsigned size = reader.u8()) reader.skip(size);
  };
  int transparent = -1;
  for (;;) {
    const unsigned block = reader.u8();
    if (block == 0x21) {
      const unsigned label = reader.u8();
      if (label == 0xF9) {
        const unsigned size = reader.u8();
        const std::uint8_t* data = reader.take(size);
        if (size >= 4 && (data[0] & 1U) != 0) transparent = data[3];
      }
      skip_blocks();
      continue;
    }
    if (block == 0x3B) fail("GIF has no image");
    if (block != 0x2C) fail("corrupt GIF");

    const unsigned left = reader.le16(), top = reader.le16();
    const unsigned frame_width = reader.le16(), frame_height = reader.le16();
    const unsigned frame_flags = reader.u8();
    std::array<std::uint32_t, 256> palette = global;
    if ((frame_flags & 0x80U) != 0) {
      const unsigned count = 2U << (frame_flags & 7U);
      for (unsigned i = 0; i < count; ++i) {
        const unsigned r = reader.u8(), g = reader.u8(), b = reader.u8();
        palette[i] = pack_rgba(r, g, b);
      }
    }
    const int minimum = reader.u8();
    if (minimum < 1 || minimum > 11) fail("corrupt GIF");
    std::vector<std::uint8_t> data;
    while (const unsigned size = reader.u8()) {
      const std::uint8_t* chunk = reader.take(size);
      data.insert(data.end(), chunk, chunk + size);
    }

    DecodedRaster image = blank_raster(screen_width != 0 ? screen_width : left + frame_width,
                                       screen_height != 0 ? screen_height : top + frame_height);
    const std::size_t frame_pixels = static_cast<std::size_t>(frame_width) * frame_height;
    if (frame_pixels > (1ULL << 28)) fail("image is too large");
    std::vector<std::int16_t> indices(frame_pixels, -1);
    std::size_t written = 0;

    // LZW, codes read least significant bit first.
    std::array<std::int16_t, 4096> prefix{};
    std::array<std::uint8_t, 4096> suffix{}, first{};
    std::array<std::uint8_t, 4096> stack{};
    const int clear = 1 << minimum, end = clear + 1;
    for (int i = 0; i < clear; ++i) {
      prefix[static_cast<std::size_t>(i)] = -1;
      suffix[static_cast<std::size_t>(i)] = first[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    }
    int size = minimum + 1, next = clear + 2, previous = -1;
    std::size_t bit = 0;
    const auto emit = [&](int code) {
      std::size_t depth = 0;
      for (int c = code; c >= 0 && depth < stack.size(); c = prefix[static_cast<std::size_t>(c)])
        stack[depth++] = suffix[static_cast<std::size_t>(c)];
      while (depth > 0 && written < frame_pixels) indices[written++] = stack[--depth];
    };
    while (written < frame_pixels && bit + static_cast<std::size_t>(size) <= data.size() * 8) {
      int code = 0;
      for (int i = 0; i < size; ++i, ++bit)
        code |= ((data[bit >> 3U] >> (bit & 7U)) & 1) << i;
      if (code == clear) {
        size = minimum + 1;
        next = clear + 2;
        previous = -1;
        continue;
      }
      if (code == end) break;
      if (previous < 0) {
        if (code >= clear) break;
        emit(code);
        previous = code;
        continue;
      }
      if (code < next) {
        if (next < 4096) {
          prefix[static_cast<std::size_t>(next)] = static_cast<std::int16_t>(previous);
          suffix[static_cast<std::size_t>(next)] = first[static_cast<std::size_t>(code)];
          first[static_cast<std::size_t>(next)] = first[static_cast<std::size_t>(previous)];
          ++next;
        }
      } else if (code == next && next < 4096) {
        prefix[static_cast<std::size_t>(next)] = static_cast<std::int16_t>(previous);
        suffix[static_cast<std::size_t>(next)] = first[static_cast<std::size_t>(previous)];
        first[static_cast<std::size_t>(next)] = first[static_cast<std::size_t>(previous)];
        ++next;
      } else {
        break;
      }
      emit(code);
      previous = code;
      if (next == (1 << size) && size < 12) ++size;
    }

    std::vector<unsigned> rows(frame_height);
    if ((frame_flags & 0x40U) != 0) {
      unsigned row = 0;
      for (const auto& [start, step] : {std::pair{0U, 8U}, std::pair{4U, 8U}, std::pair{2U, 4U}, std::pair{1U, 2U}})
        for (unsigned y = start; y < frame_height; y += step) rows[row++] = y;
    } else {
      for (unsigned y = 0; y < frame_height; ++y) rows[y] = y;
    }
    const auto canvas_width = static_cast<unsigned>(image.width);
    const auto canvas_height = static_cast<unsigned>(image.height);
    for (unsigned row = 0; row < frame_height; ++row) {
      const unsigned y = top + rows[row];
      if (y >= canvas_height) continue;
      for (unsigned x = 0; x < frame_width; ++x) {
        const int index = indices[static_cast<std::size_t>(row) * frame_width + x];
        if (index < 0 || index == transparent || left + x >= canvas_width) continue;
        image.rgba[static_cast<std::size_t>(y) * canvas_width + left + x] = palette[static_cast<std::size_t>(index)];
      }
    }
    return image;
  }
}

// ---------------------------------------------------------------------------
// BMP

// A masked channel scaled to `bits` bits.
std::uint64_t mask_channel(std::uint32_t value, std::uint32_t mask, std::uint64_t missing, int bits) {
  if (mask == 0) return missing;
  const int shift = std::countr_zero(mask);
  const int width = std::popcount(mask);
  const std::uint64_t channel = (value & mask) >> shift;
  const std::uint64_t maximum = (1ULL << width) - 1U;
  const std::uint64_t target = (1ULL << bits) - 1U;
  return (channel * target + maximum / 2U) / maximum;
}

DecodedRaster decode_bmp(std::span<const std::uint8_t> bytes) {
  Reader reader(bytes);
  reader.skip(10);
  const std::uint32_t data_offset = reader.le32();
  const std::uint32_t header_size = reader.le32();
  if (header_size != 12 && header_size != 40 && header_size != 52 && header_size != 56 && header_size != 108 &&
      header_size != 124)
    fail("unsupported BMP header");
  std::int64_t width = 0, height = 0;
  unsigned bpp = 0;
  std::uint32_t compression = 0, colors_used = 0;
  if (header_size == 12) {
    width = reader.le16();
    height = reader.le16();
    if (reader.le16() != 1) fail("invalid BMP");
    bpp = reader.le16();
  } else {
    width = static_cast<std::int32_t>(reader.le32());
    height = static_cast<std::int32_t>(reader.le32());
    if (reader.le16() != 1) fail("invalid BMP");
    bpp = reader.le16();
    compression = reader.le32();
    reader.skip(12);
    colors_used = reader.le32();
    reader.skip(4);
  }
  if (compression == 1 || compression == 2) fail("RLE BMP is not supported");
  if (compression != 0 && compression != 3 && compression != 6) fail("unsupported BMP compression");
  if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) fail("unsupported BMP depth");
  const bool bottom_up = height > 0;
  height = height < 0 ? -height : height;
  if (width <= 0) fail("invalid BMP size");

  std::uint32_t red = 0, green = 0, blue = 0, alpha = 0;
  if (compression == 0) {
    if (bpp == 16) {
      red = 0x7C00;
      green = 0x03E0;
      blue = 0x001F;
    } else if (bpp == 32) {
      red = 0x00FF0000;
      green = 0x0000FF00;
      blue = 0x000000FF;
      alpha = 0xFF000000;
    }
  } else if (bpp == 16 || bpp == 32) {
    red = reader.le32();
    green = reader.le32();
    blue = reader.le32();
    if (header_size >= 56 || compression == 6) alpha = reader.le32();
  }
  std::array<std::uint32_t, 256> palette{};
  palette.fill(pack_rgba(0, 0, 0));
  if (bpp <= 8) {
    reader.seek(14 + header_size);
    const std::uint32_t entries = std::min<std::uint32_t>(colors_used != 0 ? colors_used : 1U << bpp, 256);
    for (std::uint32_t i = 0; i < entries; ++i) {
      const unsigned b = reader.u8(), g = reader.u8(), r = reader.u8();
      if (header_size != 12) reader.skip(1);
      palette[i] = pack_rgba(r, g, b);
    }
  }

  DecodedRaster image = blank_raster(static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height));
  // Masks wider than eight bits (10-bit BMPs) keep their precision.
  const bool deep = (bpp == 16 || bpp == 32) &&
                    std::max({std::popcount(red), std::popcount(green), std::popcount(blue), std::popcount(alpha)}) > 8;
  if (deep) {
    image.rgba16.assign(image.rgba.size(), 0);
    image.rgba = {};
  }
  reader.seek(data_offset);
  const std::size_t stride = (static_cast<std::size_t>(width) * bpp + 31) / 32 * 4;
  bool any_alpha = false;
  for (std::int64_t y = 0; y < height; ++y) {
    const std::uint8_t* row = reader.take(stride);
    const std::size_t line = static_cast<std::size_t>(bottom_up ? height - 1 - y : y) * static_cast<std::size_t>(width);
    for (std::int64_t x = 0; x < width; ++x) {
      const auto ux = static_cast<std::size_t>(x);
      std::uint32_t pixel = 0;
      if (bpp <= 8) {
        const std::size_t bit = ux * bpp;
        pixel = palette[(row[bit >> 3U] >> (8 - bpp - (bit & 7U))) & ((1U << bpp) - 1U)];
      } else if (bpp == 24) {
        pixel = pack_rgba(row[ux * 3 + 2], row[ux * 3 + 1], row[ux * 3]);
      } else {
        const std::uint32_t value = bpp == 16
            ? static_cast<std::uint32_t>(row[ux * 2] | row[ux * 2 + 1] << 8)
            : static_cast<std::uint32_t>(row[ux * 4]) | static_cast<std::uint32_t>(row[ux * 4 + 1]) << 8U |
                  static_cast<std::uint32_t>(row[ux * 4 + 2]) << 16U | static_cast<std::uint32_t>(row[ux * 4 + 3]) << 24U;
        const int bits = deep ? 16 : 8;
        const std::uint64_t a = mask_channel(value, alpha, (1ULL << bits) - 1U, bits);
        any_alpha = any_alpha || a != 0;
        const std::uint64_t r = mask_channel(value, red, 0, bits), g = mask_channel(value, green, 0, bits),
                            b = mask_channel(value, blue, 0, bits);
        if (deep) {
          image.rgba16[line + ux] = pack_rgba16(r, g, b, a);
          continue;
        }
        pixel = pack_rgba(static_cast<unsigned>(r), static_cast<unsigned>(g), static_cast<unsigned>(b), static_cast<unsigned>(a));
      }
      image.rgba[line + ux] = pixel;
    }
  }
  // Plenty of writers leave the alpha byte zero: such files are opaque.
  if (alpha != 0 && !any_alpha) {
    for (std::uint32_t& pixel : image.rgba) pixel |= 0xFF000000U;
    for (std::uint64_t& pixel : image.rgba16) pixel |= 0xFFFF000000000000ULL;
  }
  return image;
}

// ---------------------------------------------------------------------------
// TGA

bool tga_header_valid(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 18) return false;
  const unsigned map_type = bytes[1], type = bytes[2], map_bits = bytes[7], bpp = bytes[16];
  const unsigned width = bytes[12] | bytes[13] << 8, height = bytes[14] | bytes[15] << 8;
  if (map_type > 1 || width == 0 || height == 0) return false;
  const unsigned base = type & 7U;
  if ((type != 1 && type != 2 && type != 3 && type != 9 && type != 10 && type != 11)) return false;
  if (base == 1) return map_type == 1 && (bpp == 8 || bpp == 16) &&
                        (map_bits == 15 || map_bits == 16 || map_bits == 24 || map_bits == 32);
  if (base == 2) return bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32;
  return bpp == 8 || bpp == 16;
}

DecodedRaster decode_tga(std::span<const std::uint8_t> bytes) {
  if (!tga_header_valid(bytes)) fail("unknown image type");
  Reader reader(bytes);
  const unsigned id_length = reader.u8();
  const unsigned map_type = reader.u8();
  const unsigned type = reader.u8();
  const unsigned map_first = reader.le16();
  const unsigned map_length = reader.le16();
  const unsigned map_bits = reader.u8();
  reader.skip(4);
  const unsigned width = reader.le16(), height = reader.le16();
  const unsigned bpp = reader.u8();
  const unsigned descriptor = reader.u8();
  reader.skip(id_length);
  const unsigned base = type & 7U;
  const auto truecolor = [](const std::uint8_t* p, unsigned bits) {
    if (bits == 15 || bits == 16) {
      const unsigned value = p[0] | p[1] << 8;
      const auto scale = [](unsigned v) { return (v * 255U + 15U) / 31U; };
      return pack_rgba(scale((value >> 10U) & 31U), scale((value >> 5U) & 31U), scale(value & 31U));
    }
    if (bits == 24) return pack_rgba(p[2], p[1], p[0]);
    return pack_rgba(p[2], p[1], p[0], p[3]);
  };
  std::vector<std::uint32_t> palette;
  if (map_type == 1) {
    const std::size_t entry = (map_bits + 7) / 8;
    for (unsigned i = 0; i < map_length; ++i) palette.push_back(truecolor(reader.take(entry), map_bits));
  }
  const std::size_t pixel_bytes = (bpp + 7) / 8;
  const auto convert = [&](const std::uint8_t* p) -> std::uint32_t {
    if (base == 1) {
      const unsigned index = (bpp == 8 ? p[0] : p[0] | p[1] << 8) - map_first;
      return index < palette.size() ? palette[index] : pack_rgba(0, 0, 0);
    }
    if (base == 2) return truecolor(p, bpp);
    return bpp == 8 ? pack_rgba(p[0], p[0], p[0]) : pack_rgba(p[0], p[0], p[0], p[1]);
  };
  DecodedRaster image = blank_raster(width, height);
  const std::size_t total = static_cast<std::size_t>(width) * height;
  std::vector<std::uint32_t> pixels;
  pixels.reserve(total);
  if (type >= 8) {
    while (pixels.size() < total) {
      const unsigned packet = reader.u8();
      const std::size_t count = std::min<std::size_t>((packet & 127U) + 1U, total - pixels.size());
      if ((packet & 128U) != 0) {
        pixels.insert(pixels.end(), count, convert(reader.take(pixel_bytes)));
      } else {
        for (std::size_t i = 0; i < count; ++i) pixels.push_back(convert(reader.take(pixel_bytes)));
      }
    }
  } else {
    for (std::size_t i = 0; i < total; ++i) pixels.push_back(convert(reader.take(pixel_bytes)));
  }
  const bool top_down = (descriptor & 0x20U) != 0;
  for (unsigned y = 0; y < height; ++y)
    std::memcpy(image.rgba.data() + static_cast<std::size_t>(top_down ? y : height - 1 - y) * width,
                pixels.data() + static_cast<std::size_t>(y) * width, width * sizeof(std::uint32_t));
  return image;
}

// ---------------------------------------------------------------------------
// PSD: the merged composite of an RGB document

DecodedRaster decode_psd(std::span<const std::uint8_t> bytes) {
  Reader reader(bytes);
  reader.skip(4);
  if (reader.be16() != 1) fail("unsupported PSD version");
  reader.skip(6);
  const unsigned channels = reader.be16();
  const std::uint32_t height = reader.be32();
  const std::uint32_t width = reader.be32();
  const unsigned depth = reader.be16();
  if (channels == 0 || channels > 56) fail("invalid PSD channel count");
  if (depth != 8 && depth != 16) fail("unsupported PSD depth");
  if (reader.be16() != 3) fail("PSD is not RGB");
  for (int section = 0; section < 3; ++section) reader.skip(reader.be32());
  const unsigned compression = reader.be16();
  if (compression > 1) fail("unsupported PSD compression");
  DecodedRaster image = blank_raster(width, height);
  const std::size_t pixels = static_cast<std::size_t>(width) * height;
  const std::size_t sample_bytes = depth / 8;
  const unsigned used = std::min(channels, 4U);
  std::array<std::vector<std::uint16_t>, 4> planes;
  if (compression == 1) reader.skip(static_cast<std::size_t>(height) * channels * 2);
  for (unsigned channel = 0; channel < used; ++channel) {
    auto& plane = planes[channel];
    plane.resize(pixels);
    if (compression == 0) {
      const std::uint8_t* data = reader.take(pixels * sample_bytes);
      for (std::size_t i = 0; i < pixels; ++i)
        plane[i] = sample_bytes == 2 ? static_cast<std::uint16_t>(data[2 * i] << 8 | data[2 * i + 1])
                                     : static_cast<std::uint16_t>(data[i] * 257U);
      continue;
    }
    std::size_t produced = 0;
    const std::size_t needed = pixels * sample_bytes;
    const auto put = [&](std::uint8_t value) {
      std::uint16_t& sample = plane[produced / sample_bytes];
      if (sample_bytes == 1) sample = static_cast<std::uint16_t>(value * 257U);
      else if (produced % 2 == 0) sample = static_cast<std::uint16_t>(value << 8);
      else sample = static_cast<std::uint16_t>(sample | value);
      ++produced;
    };
    while (produced < needed) {
      const int header = static_cast<std::int8_t>(reader.u8());
      if (header >= 0) {
        for (int i = 0; i <= header && produced < needed; ++i) put(reader.u8());
      } else if (header != -128) {
        const std::uint8_t value = reader.u8();
        for (int i = 0; i < 1 - header && produced < needed; ++i) put(value);
      }
    }
  }
  image.rgba16.assign(pixels, 0);
  image.rgba = {};
  for (std::size_t i = 0; i < pixels; ++i) {
    std::array<float, 4> c{0, 0, 0, 65535};
    for (unsigned channel = 0; channel < used; ++channel) c[channel] = planes[channel][i];
    if (used == 4 && c[3] != 0 && c[3] != 65535) {
      // Photoshop composites partly transparent pixels over white.
      const float a = c[3] / 65535.0F;
      for (std::size_t k = 0; k < 3; ++k) c[k] = (c[k] - 65535.0F * (1.0F - a)) / a;
    }
    const auto to16 = [](float v) { return static_cast<std::uint64_t>(std::clamp(v, 0.0F, 65535.0F) + 0.5F); };
    image.rgba16[i] = pack_rgba16(to16(c[0]), to16(c[1]), to16(c[2]), to16(c[3]));
  }
  return image;
}

// ---------------------------------------------------------------------------
// Radiance HDR, tone mapped with gamma 2.2

DecodedRaster decode_hdr(std::span<const std::uint8_t> bytes) {
  std::size_t pos = 0;
  const auto line = [&] {
    std::string text;
    while (pos < bytes.size() && bytes[pos] != '\n') text.push_back(static_cast<char>(bytes[pos++]));
    if (pos >= bytes.size()) fail("truncated HDR header");
    ++pos;
    return text;
  };
  (void)line();
  bool format = false;
  for (std::string text = line(); !text.empty(); text = line()) {
    if (text.starts_with("FORMAT=")) {
      if (text != "FORMAT=32-bit_rle_rgbe") fail("unsupported HDR format");
      format = true;
    }
  }
  if (!format) fail("unsupported HDR format");
  const std::string resolution = line();
  unsigned long long height = 0, width = 0;
  char tail = 0;
  if (std::sscanf(resolution.c_str(), "-Y %llu +X %llu%c", &height, &width, &tail) != 2)
    fail("unsupported HDR orientation");
  DecodedRaster image = blank_raster(width, height);
  Reader reader(bytes);
  reader.seek(pos);
  image.rgba16.assign(image.rgba.size(), 0);
  image.rgba = {};
  std::vector<std::uint8_t> scanline(static_cast<std::size_t>(width) * 4);
  for (unsigned long long y = 0; y < height; ++y) {
    const bool rle = width >= 8 && width < 32768 && reader.left() >= 4 && bytes[reader.pos()] == 2 &&
                     bytes[reader.pos() + 1] == 2 && (bytes[reader.pos() + 2] & 0x80U) == 0;
    if (rle) {
      reader.skip(2);
      if (reader.be16() != width) fail("corrupt HDR scanline");
      for (std::size_t channel = 0; channel < 4; ++channel) {
        std::size_t x = 0;
        while (x < width) {
          unsigned count = reader.u8();
          if (count > 128) {
            count -= 128;
            if (count == 0 || x + count > width) fail("corrupt HDR scanline");
            const std::uint8_t value = reader.u8();
            for (unsigned i = 0; i < count; ++i) scanline[(x++) * 4 + channel] = value;
          } else {
            if (count == 0 || x + count > width) fail("corrupt HDR scanline");
            for (unsigned i = 0; i < count; ++i) scanline[(x++) * 4 + channel] = reader.u8();
          }
        }
      }
    } else {
      std::memcpy(scanline.data(), reader.take(scanline.size()), scanline.size());
    }
    for (std::size_t x = 0; x < width; ++x) {
      const std::uint8_t* p = scanline.data() + x * 4;
      std::array<std::uint64_t, 3> c{};
      if (p[3] != 0) {
        const float scale = std::ldexp(1.0F, static_cast<int>(p[3]) - 136);
        for (std::size_t k = 0; k < 3; ++k)
          c[k] = static_cast<std::uint64_t>(
              std::min(65535.0F, std::pow(static_cast<float>(p[k]) * scale, 1.0F / 2.2F) * 65535.0F + 0.5F));
      }
      image.rgba16[static_cast<std::size_t>(y) * width + x] = pack_rgba16(c[0], c[1], c[2]);
    }
  }
  return image;
}

// ---------------------------------------------------------------------------
// Binary PNM: P5 (gray) and P6 (RGB)

DecodedRaster decode_pnm(std::span<const std::uint8_t> bytes) {
  const bool color = bytes[1] == '6';
  std::size_t pos = 2;
  const auto number = [&] {
    for (;;) {
      while (pos < bytes.size() && std::isspace(bytes[pos]) != 0) ++pos;
      if (pos < bytes.size() && bytes[pos] == '#') {
        while (pos < bytes.size() && bytes[pos] != '\n') ++pos;
        continue;
      }
      break;
    }
    std::uint64_t value = 0;
    const std::size_t start = pos;
    while (pos < bytes.size() && bytes[pos] >= '0' && bytes[pos] <= '9' && value < (1ULL << 32))
      value = value * 10 + (bytes[pos++] - '0');
    if (pos == start) fail("corrupt PNM header");
    return value;
  };
  const std::uint64_t width = number(), height = number(), maximum = number();
  if (maximum == 0 || maximum > 65535) fail("unsupported PNM maximum value");
  ++pos;
  DecodedRaster image = blank_raster(width, height);
  Reader reader(bytes);
  reader.seek(pos);
  const std::size_t sample_bytes = maximum > 255 ? 2 : 1;
  const std::size_t samples = color ? 3 : 1;
  const std::size_t count = image.rgba.size();
  const std::uint8_t* data = reader.take(count * samples * sample_bytes);
  const bool deep = maximum > 255;
  if (deep) {
    image.rgba16.assign(count, 0);
    image.rgba = {};
  }
  const std::uint64_t target = deep ? 65535 : 255;
  for (std::size_t i = 0; i < count; ++i) {
    std::array<std::uint64_t, 3> c{};
    for (std::size_t k = 0; k < samples; ++k) {
      const std::uint8_t* p = data + (i * samples + k) * sample_bytes;
      const std::uint64_t value = sample_bytes == 2 ? static_cast<std::uint64_t>(p[0] << 8 | p[1]) : p[0];
      c[k] = (std::min(value, maximum) * target + maximum / 2) / maximum;
    }
    if (!color) c[1] = c[2] = c[0];
    if (deep) image.rgba16[i] = pack_rgba16(c[0], c[1], c[2]);
    else image.rgba[i] = pack_rgba(static_cast<unsigned>(c[0]), static_cast<unsigned>(c[1]), static_cast<unsigned>(c[2]));
  }
  return image;
}

}  // namespace

// ---------------------------------------------------------------------------
// PNG encoding: per-row filters picked by the smallest sum of absolute
// differences, deflated with LZ77 (hash chains) and the fixed Huffman codes.

namespace {

class LsbWriter {
 public:
  std::vector<std::uint8_t> out;
  void bits(std::uint32_t value, int count) {
    buffer_ |= static_cast<std::uint64_t>(value) << count_;
    count_ += count;
    while (count_ >= 8) {
      out.push_back(static_cast<std::uint8_t>(buffer_));
      buffer_ >>= 8;
      count_ -= 8;
    }
  }
  // Huffman codes go most significant bit first.
  void code(std::uint32_t value, int length) {
    std::uint32_t reversed = 0;
    for (int i = 0; i < length; ++i) reversed |= ((value >> i) & 1U) << (length - 1 - i);
    bits(reversed, length);
  }
  void flush() {
    if (count_ > 0) bits(0, 8 - count_);
  }

 private:
  std::uint64_t buffer_ = 0;
  int count_ = 0;
};

void literal_code(LsbWriter& out, unsigned symbol) {
  if (symbol < 144) out.code(0x30 + symbol, 8);
  else if (symbol < 256) out.code(0x190 + symbol - 144, 9);
  else if (symbol < 280) out.code(symbol - 256, 7);
  else out.code(0xC0 + symbol - 280, 8);
}

std::vector<std::uint8_t> deflate_fixed(const std::vector<std::uint8_t>& data) {
  static constexpr std::array<std::uint16_t, 29> length_base{
      3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static constexpr std::array<std::uint8_t, 29> length_extra{
      0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static constexpr std::array<std::uint16_t, 30> distance_base{
      1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
      257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static constexpr std::array<std::uint8_t, 30> distance_extra{
      0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  constexpr std::size_t window = 32768, max_chain = 48;
  constexpr int hash_bits = 15;
  LsbWriter out;
  out.bits(1, 1);  // final block
  out.bits(1, 2);  // fixed codes
  std::vector<std::int32_t> head(1U << hash_bits, -1);
  std::vector<std::int32_t> previous(data.size(), -1);
  const auto hash = [&](std::size_t i) {
    return ((static_cast<std::uint32_t>(data[i]) << 16U | static_cast<std::uint32_t>(data[i + 1]) << 8U | data[i + 2]) *
            2654435761U) >> (32 - hash_bits);
  };
  const auto insert = [&](std::size_t i) {
    if (i + 2 >= data.size()) return;
    const std::uint32_t h = hash(i);
    previous[i] = head[h];
    head[h] = static_cast<std::int32_t>(i);
  };
  std::size_t i = 0;
  while (i < data.size()) {
    std::size_t best_length = 0, best_distance = 0;
    if (i + 2 < data.size()) {
      std::int32_t candidate = head[hash(i)];
      for (std::size_t chain = 0; candidate >= 0 && chain < max_chain; ++chain) {
        const auto start = static_cast<std::size_t>(candidate);
        if (i - start > window) break;
        std::size_t length = 0;
        const std::size_t limit = std::min<std::size_t>(258, data.size() - i);
        while (length < limit && data[start + length] == data[i + length]) ++length;
        if (length > best_length) {
          best_length = length;
          best_distance = i - start;
          if (length == limit) break;
        }
        candidate = previous[start];
      }
    }
    if (best_length >= 3) {
      std::size_t code = 0;
      while (code + 1 < length_base.size() && length_base[code + 1] <= best_length) ++code;
      literal_code(out, static_cast<unsigned>(257 + code));
      out.bits(static_cast<std::uint32_t>(best_length - length_base[code]), length_extra[code]);
      std::size_t distance_code = 0;
      while (distance_code + 1 < distance_base.size() && distance_base[distance_code + 1] <= best_distance) ++distance_code;
      out.code(static_cast<std::uint32_t>(distance_code), 5);
      out.bits(static_cast<std::uint32_t>(best_distance - distance_base[distance_code]), distance_extra[distance_code]);
      for (std::size_t k = 0; k < best_length; ++k) insert(i + k);
      i += best_length;
    } else {
      literal_code(out, data[i]);
      insert(i);
      ++i;
    }
  }
  literal_code(out, 256);
  out.flush();
  return std::move(out.out);
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t n = 0; n < 256; ++n) {
      std::uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
      t[n] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 255U] ^ (crc >> 8U);
  return ~crc;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void chunk(std::vector<std::uint8_t>& out, const char (&kind)[5], const std::vector<std::uint8_t>& data) {
  put32(out, static_cast<std::uint32_t>(data.size()));
  const std::size_t start = out.size();
  out.insert(out.end(), kind, kind + 4);
  out.insert(out.end(), data.begin(), data.end());
  put32(out, crc32(out.data() + start, out.size() - start));
}

}  // namespace

std::vector<std::uint8_t> encode_png(std::uint32_t width, std::uint32_t height, const std::uint32_t* rgba,
                                     const std::uint64_t* rgba16) {
  const bool deep = rgba16 != nullptr;
  const std::size_t bpp = deep ? 8 : 4;
  const std::size_t stride = static_cast<std::size_t>(width) * bpp;
  std::vector<std::uint8_t> raw(stride), previous(stride, 0), filtered((stride + 1) * height);
  std::array<std::vector<std::uint8_t>, 5> candidates;
  for (auto& candidate : candidates) candidate.resize(stride);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * width + x;
      std::uint8_t* p = raw.data() + static_cast<std::size_t>(x) * bpp;
      if (deep) {
        for (int c = 0; c < 4; ++c) {
          const auto value = static_cast<std::uint16_t>(rgba16[i] >> (16 * c));
          p[2 * c] = static_cast<std::uint8_t>(value >> 8U);
          p[2 * c + 1] = static_cast<std::uint8_t>(value);
        }
      } else {
        for (int c = 0; c < 4; ++c) p[c] = static_cast<std::uint8_t>(rgba[i] >> (8 * c));
      }
    }
    std::size_t best = 0, best_cost = ~std::size_t{0};
    for (std::size_t filter = 0; filter < 5; ++filter) {
      std::size_t cost = 0;
      for (std::size_t k = 0; k < stride; ++k) {
        const int a = k >= bpp ? raw[k - bpp] : 0, b = previous[k], c = k >= bpp ? previous[k - bpp] : 0;
        int predicted = 0;
        switch (filter) {
          case 1: predicted = a; break;
          case 2: predicted = b; break;
          case 3: predicted = (a + b) / 2; break;
          case 4: {
            const int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
            predicted = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            break;
          }
          default: break;
        }
        const auto value = static_cast<std::uint8_t>(raw[k] - predicted);
        candidates[filter][k] = value;
        cost += static_cast<std::size_t>(std::abs(static_cast<std::int8_t>(value)));
      }
      if (cost < best_cost) {
        best_cost = cost;
        best = filter;
      }
    }
    std::uint8_t* row = filtered.data() + static_cast<std::size_t>(y) * (stride + 1);
    row[0] = static_cast<std::uint8_t>(best);
    std::memcpy(row + 1, candidates[best].data(), stride);
    std::swap(previous, raw);
  }
  std::vector<std::uint8_t> zlib{0x78, 0x01};
  const std::vector<std::uint8_t> deflated = deflate_fixed(filtered);
  zlib.insert(zlib.end(), deflated.begin(), deflated.end());
  std::uint32_t a = 1, b = 0;
  for (const std::uint8_t byte : filtered) {
    a = (a + byte) % 65521U;
    b = (b + a) % 65521U;
  }
  put32(zlib, b << 16U | a);

  std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<std::uint8_t> header;
  put32(header, width);
  put32(header, height);
  header.insert(header.end(), {static_cast<std::uint8_t>(deep ? 16 : 8), 6, 0, 0, 0});
  chunk(png, "IHDR", header);
  chunk(png, "IDAT", zlib);
  chunk(png, "IEND", {});
  return png;
}

bool is_webp(std::span<const std::uint8_t> bytes) {
  return bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
         std::memcmp(bytes.data() + 8, "WEBP", 4) == 0;
}

namespace {

DecodedRaster decode_any(std::span<const std::uint8_t> bytes) {
  const auto starts = [&](std::string_view signature) {
    return bytes.size() >= signature.size() && std::memcmp(bytes.data(), signature.data(), signature.size()) == 0;
  };
  if (starts("\x89PNG\r\n\x1a\n")) return decode_png(bytes);
  if (starts("\xFF\xD8\xFF")) return decode_jpeg(bytes);
  if (is_webp(bytes)) return decode_webp(bytes);
  if (starts("GIF87a") || starts("GIF89a")) return decode_gif(bytes);
  if (starts("BM")) return decode_bmp(bytes);
  if (starts("8BPS")) return decode_psd(bytes);
  if (starts("#?RADIANCE\n") || starts("#?RGBE\n")) return decode_hdr(bytes);
  if (starts("P5") || starts("P6")) return decode_pnm(bytes);
  return decode_tga(bytes);
}

}  // namespace

DecodedRaster decode_raster(std::span<const std::uint8_t> bytes) {
  DecodedRaster image = decode_any(bytes);
  narrow_if_exact(image);
  return image;
}

}  // namespace vw::detail
