// vw's SVG renderer: its own XML and CSS parsing, drawn with cairo. Covers
// what icons and wallpapers use: shapes and paths, transforms, fills and
// strokes (dashes, joins, caps), linear and radial gradients, group opacity,
// clip paths, luminance masks, use/symbol, nested svg, embedded or linked
// raster images, text through vw's text renderer, and Gaussian blur.
#include "image_decode_internal.hpp"
#include "text_internal.hpp"

#include <cairo.h>
#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vw::detail {
namespace {

// ---------------------------------------------------------------------------
// XML

struct Node {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::vector<std::unique_ptr<Node>> children;
  // Character data directly inside, and each child's position in it, for text.
  std::string text;
  std::vector<std::pair<std::size_t, Node*>> text_children;
  Node* parent = nullptr;

  const std::string* attribute(std::string_view key) const {
    for (const auto& [name, value] : attributes)
      if (name == key) return &value;
    return nullptr;
  }
  std::string_view get(std::string_view key) const {
    const std::string* value = attribute(key);
    return value == nullptr ? std::string_view{} : std::string_view(*value);
  }
};

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

std::string_view trim(std::string_view value) {
  while (!value.empty() && is_space(value.front())) value.remove_prefix(1);
  while (!value.empty() && is_space(value.back())) value.remove_suffix(1);
  return value;
}

std::string lowercase(std::string_view value) {
  std::string result(value);
  for (char& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return result;
}

void append_utf8(std::string& out, std::uint32_t code) {
  if (code < 0x80) {
    out.push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else if (code < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else if (code < 0x110000) {
    out.push_back(static_cast<char>(0xF0 | (code >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
}

class XmlParser {
 public:
  explicit XmlParser(std::string_view text) : text_(text) {}

  std::unique_ptr<Node> parse() {
    auto document = std::make_unique<Node>();
    Node* current = document.get();
    while (pos_ < text_.size()) {
      if (text_[pos_] != '<') {
        const std::size_t end = std::min(text_.find('<', pos_), text_.size());
        current->text += decode(text_.substr(pos_, end - pos_));
        pos_ = end;
        continue;
      }
      if (starts("<!--")) {
        skip_past("-->");
      } else if (starts("<![CDATA[")) {
        const std::size_t start = pos_ + 9;
        skip_past("]]>");
        current->text.append(text_.substr(start, pos_ - 3 - start));
      } else if (starts("<?")) {
        skip_past("?>");
      } else if (starts("<!DOCTYPE") || starts("<!doctype")) {
        doctype();
      } else if (starts("</")) {
        pos_ += 2;
        const std::string name = read_name();
        skip_past(">");
        if (current->parent != nullptr && local_name(name) == current->name) current = current->parent;
      } else if (starts("<!")) {
        skip_past(">");
      } else {
        ++pos_;
        auto node = std::make_unique<Node>();
        node->name = local_name(read_name());
        node->parent = current;
        bool closed = false;
        for (;;) {
          skip_spaces();
          if (pos_ >= text_.size()) fail("malformed SVG");
          if (starts("/>")) {
            pos_ += 2;
            closed = true;
            break;
          }
          if (text_[pos_] == '>') {
            ++pos_;
            break;
          }
          std::string attribute = read_name();
          if (attribute.empty()) fail("malformed SVG");
          skip_spaces();
          std::string value;
          if (pos_ < text_.size() && text_[pos_] == '=') {
            ++pos_;
            skip_spaces();
            if (pos_ >= text_.size()) fail("malformed SVG");
            const char quote = text_[pos_];
            if (quote == '"' || quote == '\'') {
              const std::size_t end = text_.find(quote, pos_ + 1);
              if (end == std::string_view::npos) fail("malformed SVG");
              value = decode(text_.substr(pos_ + 1, end - pos_ - 1));
              pos_ = end + 1;
            } else {
              const std::size_t start = pos_;
              while (pos_ < text_.size() && !is_space(text_[pos_]) && text_[pos_] != '>') ++pos_;
              value = decode(text_.substr(start, pos_ - start));
            }
          }
          if (attribute == "xlink:href") attribute = "href";
          node->attributes.emplace_back(std::move(attribute), std::move(value));
        }
        Node* raw = node.get();
        current->text_children.emplace_back(current->text.size(), raw);
        current->children.push_back(std::move(node));
        if (!closed) current = raw;
      }
    }
    return document;
  }

 private:
  static std::string local_name(std::string_view name) {
    const std::size_t colon = name.find(':');
    if (colon != std::string_view::npos && name.substr(0, colon) == "svg") name.remove_prefix(colon + 1);
    return std::string(name);
  }
  bool starts(std::string_view prefix) const { return text_.substr(pos_, prefix.size()) == prefix; }
  void skip_past(std::string_view end) {
    const std::size_t found = text_.find(end, pos_);
    pos_ = found == std::string_view::npos ? text_.size() : found + end.size();
  }
  void skip_spaces() {
    while (pos_ < text_.size() && is_space(text_[pos_])) ++pos_;
  }
  std::string read_name() {
    const std::size_t start = pos_;
    while (pos_ < text_.size() && !is_space(text_[pos_]) && text_[pos_] != '=' && text_[pos_] != '>' &&
           text_[pos_] != '/' && text_[pos_] != '"' && text_[pos_] != '\'')
      ++pos_;
    return std::string(text_.substr(start, pos_ - start));
  }
  // <!DOCTYPE svg ... [ <!ENTITY name "value"> ... ]>
  void doctype() {
    pos_ += 9;
    while (pos_ < text_.size() && text_[pos_] != '[' && text_[pos_] != '>') ++pos_;
    if (pos_ < text_.size() && text_[pos_] == '[') {
      ++pos_;
      while (pos_ < text_.size() && text_[pos_] != ']') {
        if (starts("<!ENTITY")) {
          pos_ += 8;
          skip_spaces();
          const std::string name = read_name();
          skip_spaces();
          if (pos_ < text_.size() && (text_[pos_] == '"' || text_[pos_] == '\'')) {
            const char quote = text_[pos_];
            const std::size_t end = text_.find(quote, pos_ + 1);
            if (end == std::string_view::npos) fail("malformed SVG");
            entities_[name] = decode(text_.substr(pos_ + 1, end - pos_ - 1));
            pos_ = end + 1;
          }
          skip_past(">");
        } else if (starts("<!--")) {
          skip_past("-->");
        } else {
          ++pos_;
        }
      }
    }
    skip_past(">");
  }
  std::string decode(std::string_view value) const {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (value[i] != '&') {
        out.push_back(value[i]);
        continue;
      }
      const std::size_t end = value.find(';', i);
      if (end == std::string_view::npos || end - i > 64) {
        out.push_back('&');
        continue;
      }
      const std::string_view name = value.substr(i + 1, end - i - 1);
      if (name == "lt") out.push_back('<');
      else if (name == "gt") out.push_back('>');
      else if (name == "amp") out.push_back('&');
      else if (name == "quot") out.push_back('"');
      else if (name == "apos") out.push_back('\'');
      else if (!name.empty() && name[0] == '#') {
        std::uint32_t code = 0;
        const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
        const std::string_view digits = name.substr(hex ? 2 : 1);
        std::from_chars(digits.data(), digits.data() + digits.size(), code, hex ? 16 : 10);
        append_utf8(out, code);
      } else if (const auto found = entities_.find(std::string(name)); found != entities_.end()) {
        out += found->second;
      } else {
        out.append(value.substr(i, end - i + 1));
      }
      i = end;
    }
    return out;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  std::unordered_map<std::string, std::string> entities_;
};

// ---------------------------------------------------------------------------
// Values

class Scanner {
 public:
  explicit Scanner(std::string_view text) : text_(text) {}
  void skip_separators() {
    while (pos_ < text_.size() && (is_space(text_[pos_]) || text_[pos_] == ',')) ++pos_;
  }
  bool done() {
    skip_separators();
    return pos_ >= text_.size();
  }
  char peek() {
    skip_separators();
    return pos_ < text_.size() ? text_[pos_] : '\0';
  }
  char next_char() { return pos_ < text_.size() ? text_[pos_++] : '\0'; }
  std::optional<double> number() {
    skip_separators();
    const std::size_t start = pos_;
    if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
    bool digits = false, dot = false;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c >= '0' && c <= '9') digits = true;
      else if (c == '.' && !dot) dot = true;
      else break;
      ++pos_;
    }
    if (!digits) {
      pos_ = start;
      return std::nullopt;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      std::size_t exponent = pos_ + 1;
      if (exponent < text_.size() && (text_[exponent] == '+' || text_[exponent] == '-')) ++exponent;
      if (exponent < text_.size() && text_[exponent] >= '0' && text_[exponent] <= '9') {
        pos_ = exponent;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
      }
    }
    std::string copy(text_.substr(start, pos_ - start));
    if (!copy.empty() && copy[0] == '+') copy.erase(0, 1);
    return std::strtod(copy.c_str(), nullptr);
  }
  // Arc flags may run together with what follows ("a1 1 0 01.5.5").
  std::optional<double> flag() {
    skip_separators();
    if (pos_ < text_.size() && (text_[pos_] == '0' || text_[pos_] == '1')) return text_[pos_++] - '0';
    return std::nullopt;
  }
  std::string_view rest() const { return text_.substr(pos_); }
  std::size_t pos() const { return pos_; }
  void set_pos(std::size_t pos) { pos_ = pos; }

 private:
  std::string_view text_;
  std::size_t pos_ = 0;
};

enum class Unit { none, px, pt, pc, mm, cm, in, em, ex, percent };

struct Length {
  double value = 0;
  Unit unit = Unit::none;
};

std::optional<Length> parse_length(std::string_view text) {
  text = trim(text);
  Scanner scanner(text);
  const std::optional<double> number = scanner.number();
  if (!number) return std::nullopt;
  const std::string unit = lowercase(trim(scanner.rest()));
  Length length{*number, Unit::none};
  if (unit.empty()) length.unit = Unit::none;
  else if (unit == "px") length.unit = Unit::px;
  else if (unit == "pt") length.unit = Unit::pt;
  else if (unit == "pc") length.unit = Unit::pc;
  else if (unit == "mm") length.unit = Unit::mm;
  else if (unit == "cm") length.unit = Unit::cm;
  else if (unit == "in") length.unit = Unit::in;
  else if (unit == "em") length.unit = Unit::em;
  else if (unit == "ex") length.unit = Unit::ex;
  else if (unit == "%") length.unit = Unit::percent;
  else return std::nullopt;
  return length;
}

struct Color {
  double r = 0, g = 0, b = 0, a = 1;
};

const std::unordered_map<std::string_view, std::uint32_t>& named_colors() {
  static const std::unordered_map<std::string_view, std::uint32_t> colors{
      {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF}, {"aquamarine", 0x7FFFD4},
      {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4}, {"black", 0x000000},
      {"blanchedalmond", 0xFFEBCD}, {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A},
      {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E},
      {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED}, {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C},
      {"cyan", 0x00FFFF}, {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B},
      {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9}, {"darkkhaki", 0xBDB76B},
      {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00}, {"darkorchid", 0x9932CC},
      {"darkred", 0x8B0000}, {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
      {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1}, {"darkviolet", 0x9400D3},
      {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
      {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22},
      {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF}, {"gold", 0xFFD700},
      {"goldenrod", 0xDAA520}, {"gray", 0x808080}, {"grey", 0x808080}, {"green", 0x008000},
      {"greenyellow", 0xADFF2F}, {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C},
      {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C}, {"lavender", 0xE6E6FA},
      {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6},
      {"lightcoral", 0xF08080}, {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3},
      {"lightgreen", 0x90EE90}, {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A},
      {"lightseagreen", 0x20B2AA}, {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
      {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00}, {"limegreen", 0x32CD32},
      {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66CDAA},
      {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB}, {"mediumseagreen", 0x3CB371},
      {"mediumslateblue", 0x7B68EE}, {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC},
      {"mediumvioletred", 0xC71585}, {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1},
      {"moccasin", 0xFFE4B5}, {"navajowhite", 0xFFDEAD}, {"navy", 0x000080}, {"oldlace", 0xFDF5E6},
      {"olive", 0x808000}, {"olivedrab", 0x6B8E23}, {"orange", 0xFFA500}, {"orangered", 0xFF4500},
      {"orchid", 0xDA70D6}, {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98}, {"paleturquoise", 0xAFEEEE},
      {"palevioletred", 0xDB7093}, {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F},
      {"pink", 0xFFC0CB}, {"plum", 0xDDA0DD}, {"powderblue", 0xB0E0E6}, {"purple", 0x800080},
      {"rebeccapurple", 0x663399}, {"red", 0xFF0000}, {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1},
      {"saddlebrown", 0x8B4513}, {"salmon", 0xFA8072}, {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57},
      {"seashell", 0xFFF5EE}, {"sienna", 0xA0522D}, {"silver", 0xC0C0C0}, {"skyblue", 0x87CEEB},
      {"slateblue", 0x6A5ACD}, {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xFFFAFA},
      {"springgreen", 0x00FF7F}, {"steelblue", 0x4682B4}, {"tan", 0xD2B48C}, {"teal", 0x008080},
      {"thistle", 0xD8BFD8}, {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE},
      {"wheat", 0xF5DEB3}, {"white", 0xFFFFFF}, {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00},
      {"yellowgreen", 0x9ACD32}};
  return colors;
}

double hue_to_rgb(double p, double q, double t) {
  if (t < 0) t += 1;
  if (t > 1) t -= 1;
  if (t < 1.0 / 6) return p + (q - p) * 6 * t;
  if (t < 0.5) return q;
  if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
  return p;
}

// CSS colours: names, #rgb[a], #rrggbb[aa], rgb[a](), hsl[a]().
std::optional<Color> parse_color(std::string_view text) {
  text = trim(text);
  if (text.empty()) return std::nullopt;
  const std::string lower = lowercase(text);
  if (lower == "transparent") return Color{0, 0, 0, 0};
  if (lower[0] == '#') {
    std::string hex = lower.substr(1);
    if (hex.size() == 3 || hex.size() == 4) {
      std::string expanded;
      for (char c : hex) expanded.append(2, c);
      hex = expanded;
    }
    if (hex.size() != 6 && hex.size() != 8) return std::nullopt;
    std::uint32_t value = 0;
    const auto result = std::from_chars(hex.data(), hex.data() + hex.size(), value, 16);
    if (result.ec != std::errc{} || result.ptr != hex.data() + hex.size()) return std::nullopt;
    if (hex.size() == 6) value = value << 8U | 0xFFU;
    return Color{((value >> 24U) & 255U) / 255.0, ((value >> 16U) & 255U) / 255.0, ((value >> 8U) & 255U) / 255.0,
                 (value & 255U) / 255.0};
  }
  const std::size_t open = lower.find('(');
  if (open != std::string::npos && lower.back() == ')') {
    const std::string function = std::string(trim(std::string_view(lower).substr(0, open)));
    std::string_view arguments = std::string_view(lower).substr(open + 1, lower.size() - open - 2);
    std::vector<std::pair<double, bool>> values;
    Scanner scanner(arguments);
    while (!scanner.done()) {
      if (scanner.peek() == '/') {
        scanner.next_char();
        continue;
      }
      const std::optional<double> number = scanner.number();
      if (!number) return std::nullopt;
      bool percent = false;
      std::string_view rest = scanner.rest();
      if (!rest.empty() && rest[0] == '%') {
        percent = true;
        scanner.set_pos(scanner.pos() + 1);
      } else if (rest.starts_with("deg")) {
        scanner.set_pos(scanner.pos() + 3);
      }
      values.emplace_back(*number, percent);
    }
    if (values.size() < 3 || values.size() > 4) return std::nullopt;
    const auto alpha = [&]() {
      if (values.size() < 4) return 1.0;
      return std::clamp(values[3].second ? values[3].first / 100 : values[3].first, 0.0, 1.0);
    };
    if (function == "rgb" || function == "rgba") {
      const auto channel = [&](std::size_t i) {
        return std::clamp(values[i].second ? values[i].first / 100 : values[i].first / 255, 0.0, 1.0);
      };
      return Color{channel(0), channel(1), channel(2), alpha()};
    }
    if (function == "hsl" || function == "hsla") {
      const double h = std::fmod(std::fmod(values[0].first, 360) + 360, 360) / 360;
      const double s = std::clamp(values[1].first / 100, 0.0, 1.0);
      const double l = std::clamp(values[2].first / 100, 0.0, 1.0);
      const double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
      const double p = 2 * l - q;
      return Color{hue_to_rgb(p, q, h + 1.0 / 3), hue_to_rgb(p, q, h), hue_to_rgb(p, q, h - 1.0 / 3), alpha()};
    }
    return std::nullopt;
  }
  if (const auto found = named_colors().find(lower); found != named_colors().end()) {
    const std::uint32_t value = found->second;
    return Color{((value >> 16U) & 255U) / 255.0, ((value >> 8U) & 255U) / 255.0, (value & 255U) / 255.0, 1};
  }
  return std::nullopt;
}

struct Matrix {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
  cairo_matrix_t cairo() const { return {a, b, c, d, e, f}; }
};

Matrix multiply(const Matrix& m, const Matrix& n) {
  return {m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b, m.a * n.c + m.c * n.d,
          m.b * n.c + m.d * n.d, m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f};
}

Matrix parse_transform(std::string_view text) {
  Matrix result;
  Scanner scanner(text);
  while (!scanner.done()) {
    std::string name;
    while (std::isalpha(static_cast<unsigned char>(scanner.peek())) != 0) name.push_back(scanner.next_char());
    if (name.empty() || scanner.peek() != '(') break;
    scanner.next_char();
    std::vector<double> v;
    while (scanner.peek() != ')' && scanner.peek() != '\0') {
      const std::optional<double> number = scanner.number();
      if (!number) return result;
      v.push_back(*number);
    }
    scanner.next_char();
    Matrix m;
    constexpr double radians = M_PI / 180.0;
    if (name == "matrix" && v.size() == 6) {
      m = {v[0], v[1], v[2], v[3], v[4], v[5]};
    } else if (name == "translate" && !v.empty()) {
      m.e = v[0];
      m.f = v.size() > 1 ? v[1] : 0;
    } else if (name == "scale" && !v.empty()) {
      m.a = v[0];
      m.d = v.size() > 1 ? v[1] : v[0];
    } else if (name == "rotate" && !v.empty()) {
      const double angle = v[0] * radians;
      const Matrix rotation{std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle), 0, 0};
      if (v.size() >= 3) {
        m = multiply(multiply(Matrix{1, 0, 0, 1, v[1], v[2]}, rotation), Matrix{1, 0, 0, 1, -v[1], -v[2]});
      } else {
        m = rotation;
      }
    } else if (name == "skewx" || name == "skewX") {
      if (!v.empty()) m.c = std::tan(v[0] * radians);
    } else if (name == "skewy" || name == "skewY") {
      if (!v.empty()) m.b = std::tan(v[0] * radians);
    } else {
      return result;
    }
    result = multiply(result, m);
  }
  return result;
}

std::vector<std::pair<std::string, std::string>> parse_declarations(std::string_view text) {
  std::vector<std::pair<std::string, std::string>> result;
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find(';', start);
    if (end == std::string_view::npos) end = text.size();
    const std::string_view declaration = text.substr(start, end - start);
    const std::size_t colon = declaration.find(':');
    if (colon != std::string_view::npos) {
      std::string_view value = trim(declaration.substr(colon + 1));
      if (const std::size_t important = value.find("!important"); important != std::string_view::npos)
        value = trim(value.substr(0, important));
      result.emplace_back(lowercase(trim(declaration.substr(0, colon))), std::string(value));
    }
    start = end + 1;
  }
  return result;
}

// ---------------------------------------------------------------------------
// CSS

struct Compound {
  std::string tag;
  std::string id;
  std::vector<std::string> classes;
};

struct Rule {
  std::vector<Compound> selector;  // descendant chain, last is the subject
  int specificity = 0;
  std::size_t order = 0;
  std::vector<std::pair<std::string, std::string>> declarations;
};

std::vector<std::string_view> split_words(std::string_view text) {
  std::vector<std::string_view> words;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && is_space(text[i])) ++i;
    const std::size_t start = i;
    while (i < text.size() && !is_space(text[i])) ++i;
    if (i > start) words.push_back(text.substr(start, i - start));
  }
  return words;
}

bool has_class(const Node& node, std::string_view name) {
  for (const std::string_view word : split_words(node.get("class")))
    if (word == name) return true;
  return false;
}

bool matches(const Compound& compound, const Node& node) {
  if (!compound.tag.empty() && compound.tag != "*" && compound.tag != node.name) return false;
  if (!compound.id.empty() && compound.id != node.get("id")) return false;
  for (const std::string& name : compound.classes)
    if (!has_class(node, name)) return false;
  return true;
}

bool matches(const Rule& rule, const Node& node) {
  if (rule.selector.empty() || !matches(rule.selector.back(), node)) return false;
  const Node* ancestor = node.parent;
  for (std::size_t i = rule.selector.size() - 1; i-- > 0;) {
    while (ancestor != nullptr && !matches(rule.selector[i], *ancestor)) ancestor = ancestor->parent;
    if (ancestor == nullptr) return false;
    ancestor = ancestor->parent;
  }
  return true;
}

void parse_stylesheet(std::string_view css, std::vector<Rule>& rules) {
  std::string text;
  for (std::size_t i = 0; i < css.size(); ++i) {
    if (css.substr(i, 2) == "/*") {
      const std::size_t end = css.find("*/", i + 2);
      if (end == std::string_view::npos) break;
      i = end + 1;
      continue;
    }
    text.push_back(css[i]);
  }
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t open = text.find('{', pos);
    if (open == std::string::npos) break;
    const std::size_t close = text.find('}', open);
    if (close == std::string::npos) break;
    const std::string_view selectors = trim(std::string_view(text).substr(pos, open - pos));
    const auto declarations = parse_declarations(std::string_view(text).substr(open + 1, close - open - 1));
    pos = close + 1;
    if (selectors.starts_with("@")) continue;
    std::size_t start = 0;
    while (start <= selectors.size()) {
      std::size_t end = selectors.find(',', start);
      if (end == std::string_view::npos) end = selectors.size();
      const std::string_view selector = trim(selectors.substr(start, end - start));
      start = end + 1;
      Rule rule;
      bool supported = !selector.empty();
      for (const std::string_view word : split_words(selector)) {
        if (word == ">") continue;
        Compound compound;
        std::size_t i = 0;
        const auto read = [&]() {
          const std::size_t from = i;
          while (i < word.size() && word[i] != '.' && word[i] != '#' && word[i] != '[' && word[i] != ':') ++i;
          return std::string(word.substr(from, i - from));
        };
        compound.tag = read();
        while (i < word.size()) {
          const char kind = word[i++];
          if (kind == '.') {
            compound.classes.push_back(read());
            rule.specificity += 10;
          } else if (kind == '#') {
            compound.id = read();
            rule.specificity += 100;
          } else {
            supported = false;
            break;
          }
        }
        if (!compound.tag.empty() && compound.tag != "*") rule.specificity += 1;
        rule.selector.push_back(std::move(compound));
      }
      if (!supported) continue;
      rule.order = rules.size();
      rule.declarations = declarations;
      rules.push_back(std::move(rule));
    }
  }
}

// ---------------------------------------------------------------------------
// Style

struct Paint {
  enum class Kind { none, color, current, url } kind = Kind::none;
  Color color;
  std::string url;
  // Drawn when the url does not name a paint server.
  Kind fallback_kind = Kind::none;
  Color fallback;
};

Paint solid_black() {
  Paint paint;
  paint.kind = Paint::Kind::color;
  paint.color = {0, 0, 0, 1};
  return paint;
}

std::optional<Paint> parse_paint(std::string_view text) {
  text = trim(text);
  Paint paint;
  if (text == "none") return paint;
  if (text == "currentColor" || text == "currentcolor") {
    paint.kind = Paint::Kind::current;
    return paint;
  }
  if (text.starts_with("url(")) {
    const std::size_t close = text.find(')');
    if (close == std::string_view::npos) return std::nullopt;
    std::string_view reference = trim(text.substr(4, close - 4));
    if (!reference.empty() && (reference.front() == '"' || reference.front() == '\'')) reference = reference.substr(1, reference.size() - 2);
    if (!reference.starts_with("#")) return std::nullopt;
    paint.kind = Paint::Kind::url;
    paint.url = std::string(reference.substr(1));
    const std::string_view fallback = trim(text.substr(close + 1));
    if (fallback == "currentColor" || fallback == "currentcolor") {
      paint.fallback_kind = Paint::Kind::current;
    } else if (const auto color = parse_color(fallback)) {
      paint.fallback_kind = Paint::Kind::color;
      paint.fallback = *color;
    }
    return paint;
  }
  const auto color = parse_color(text);
  if (!color) return std::nullopt;
  paint.kind = Paint::Kind::color;
  paint.color = *color;
  return paint;
}

struct Style {
  // Inherited.
  Paint fill = solid_black();
  double fill_opacity = 1;
  bool fill_evenodd = false;
  Paint stroke;
  double stroke_opacity = 1;
  Length stroke_width{1, Unit::none};
  cairo_line_cap_t line_cap = CAIRO_LINE_CAP_BUTT;
  cairo_line_join_t line_join = CAIRO_LINE_JOIN_MITER;
  double miter_limit = 4;
  std::vector<Length> dashes;
  Length dash_offset;
  Color color{0, 0, 0, 1};
  bool visible = true;
  bool clip_evenodd = false;
  double font_size = 16;
  std::string font_family = "sans-serif";
  int font_weight = 400;
  std::string text_anchor = "start";
  // Not inherited.
  double opacity = 1;
  bool displayed = true;
  std::string clip_path;
  std::string mask;
  std::string filter;
  Color stop_color{0, 0, 0, 1};
  double stop_opacity = 1;
};

std::string url_reference(std::string_view value) {
  value = trim(value);
  if (!value.starts_with("url(")) return {};
  const std::size_t close = value.find(')');
  if (close == std::string_view::npos) return {};
  std::string_view reference = trim(value.substr(4, close - 4));
  if (!reference.empty() && (reference.front() == '"' || reference.front() == '\'')) reference = reference.substr(1, reference.size() - 2);
  return reference.starts_with("#") ? std::string(reference.substr(1)) : std::string{};
}

double parse_opacity(std::string_view value, double fallback) {
  value = trim(value);
  Scanner scanner(value);
  const std::optional<double> number = scanner.number();
  if (!number) return fallback;
  return std::clamp(trim(scanner.rest()) == "%" ? *number / 100 : *number, 0.0, 1.0);
}

void apply_property(Style& style, const Style& parent, std::string_view name, std::string_view value) {
  value = trim(value);
  if (value == "inherit") {
    if (name == "fill") style.fill = parent.fill;
    else if (name == "stroke") style.stroke = parent.stroke;
    else if (name == "color") style.color = parent.color;
    else if (name == "opacity") style.opacity = parent.opacity;
    return;
  }
  if (name == "fill") {
    if (auto paint = parse_paint(value)) style.fill = *paint;
  } else if (name == "stroke") {
    if (auto paint = parse_paint(value)) style.stroke = *paint;
  } else if (name == "fill-opacity") {
    style.fill_opacity = parse_opacity(value, style.fill_opacity);
  } else if (name == "stroke-opacity") {
    style.stroke_opacity = parse_opacity(value, style.stroke_opacity);
  } else if (name == "opacity") {
    style.opacity = parse_opacity(value, style.opacity);
  } else if (name == "fill-rule") {
    style.fill_evenodd = value == "evenodd";
  } else if (name == "clip-rule") {
    style.clip_evenodd = value == "evenodd";
  } else if (name == "stroke-width") {
    if (auto length = parse_length(value)) style.stroke_width = *length;
  } else if (name == "stroke-linecap") {
    style.line_cap = value == "round" ? CAIRO_LINE_CAP_ROUND : value == "square" ? CAIRO_LINE_CAP_SQUARE : CAIRO_LINE_CAP_BUTT;
  } else if (name == "stroke-linejoin") {
    style.line_join = value == "round" ? CAIRO_LINE_JOIN_ROUND : value == "bevel" ? CAIRO_LINE_JOIN_BEVEL : CAIRO_LINE_JOIN_MITER;
  } else if (name == "stroke-miterlimit") {
    Scanner scanner(value);
    if (auto number = scanner.number()) style.miter_limit = std::max(1.0, *number);
  } else if (name == "stroke-dasharray") {
    style.dashes.clear();
    if (value != "none") {
      std::string text(value);
      for (char& c : text)
        if (c == ',') c = ' ';
      for (const std::string_view word : split_words(text))
        if (auto length = parse_length(word)) style.dashes.push_back(*length);
    }
  } else if (name == "stroke-dashoffset") {
    if (auto length = parse_length(value)) style.dash_offset = *length;
  } else if (name == "color") {
    if (auto color = parse_color(value)) style.color = *color;
  } else if (name == "display") {
    style.displayed = value != "none";
  } else if (name == "visibility") {
    style.visible = value == "visible";
  } else if (name == "clip-path") {
    style.clip_path = url_reference(value);
  } else if (name == "mask") {
    style.mask = url_reference(value);
  } else if (name == "filter") {
    style.filter = url_reference(value);
  } else if (name == "stop-color") {
    if (value == "currentColor" || value == "currentcolor") style.stop_color = style.color;
    else if (auto color = parse_color(value)) style.stop_color = *color;
  } else if (name == "stop-opacity") {
    style.stop_opacity = parse_opacity(value, style.stop_opacity);
  } else if (name == "font-size") {
    if (auto length = parse_length(value)) {
      switch (length->unit) {
        case Unit::em: style.font_size = parent.font_size * length->value; break;
        case Unit::ex: style.font_size = parent.font_size * length->value / 2; break;
        case Unit::percent: style.font_size = parent.font_size * length->value / 100; break;
        case Unit::pt: style.font_size = length->value * 4.0 / 3.0; break;
        default: style.font_size = length->value;
      }
    }
  } else if (name == "font-family") {
    std::string family(value);
    family.erase(std::remove(family.begin(), family.end(), '\''), family.end());
    family.erase(std::remove(family.begin(), family.end(), '"'), family.end());
    style.font_family = family;
  } else if (name == "font-weight") {
    if (value == "bold" || value == "bolder") style.font_weight = 700;
    else if (value == "normal") style.font_weight = 400;
    else if (value == "lighter") style.font_weight = 300;
    else {
      Scanner scanner(value);
      if (auto number = scanner.number()) style.font_weight = static_cast<int>(*number);
    }
  } else if (name == "text-anchor") {
    style.text_anchor = std::string(value);
  } else if (name == "font") {
    for (const std::string_view word : split_words(value)) {
      if (word == "bold") style.font_weight = 700;
      else if (auto length = parse_length(word); length && length->unit != Unit::none)
        apply_property(style, parent, "font-size", word);
    }
  }
}

constexpr std::array<std::string_view, 27> presentation_attributes{
    "fill", "stroke", "fill-opacity", "stroke-opacity", "opacity", "fill-rule", "clip-rule", "stroke-width",
    "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray", "stroke-dashoffset", "color",
    "display", "visibility", "clip-path", "mask", "filter", "stop-color", "stop-opacity", "font-size",
    "font-family", "font-weight", "text-anchor", "font", "solid-color"};

// ---------------------------------------------------------------------------
// Drawing

struct Viewport {
  double width = 0, height = 0;
};

struct Bounds {
  double x = 0, y = 0, width = 0, height = 0;
};

class Renderer {
 public:
  Renderer(Node& root, std::filesystem::path base) : root_(root), base_(std::move(base)) {
    index(root_);
  }

  // The document's own size, when it states one.
  std::optional<std::pair<double, double>> intrinsic_size() const {
    const Node* svg = document();
    if (svg == nullptr) return std::nullopt;
    const auto width = parse_length(svg->get("width"));
    const auto height = parse_length(svg->get("height"));
    const auto box = view_box(*svg);
    const auto absolute = [](const std::optional<Length>& length) {
      return length && length->unit != Unit::percent && length->value > 0;
    };
    if (absolute(width) && absolute(height)) return std::pair{to_pixels(*width, 16), to_pixels(*height, 16)};
    if (box) {
      if (absolute(width)) return std::pair{to_pixels(*width, 16), to_pixels(*width, 16) * box->height / box->width};
      if (absolute(height)) return std::pair{to_pixels(*height, 16) * box->width / box->height, to_pixels(*height, 16)};
      return std::pair{box->width, box->height};
    }
    return std::nullopt;
  }

  void render(cairo_t* cr, double width, double height) {
    Node* svg = document();
    if (svg == nullptr) fail("not an SVG document");
    cr_ = cr;
    Style root_style;
    const Style style = computed(*svg, root_style);
    if (!style.displayed) return;
    Matrix m;
    const auto intrinsic = intrinsic_size();
    if (const auto box = view_box(*svg)) {
      m = fit(*box, Bounds{0, 0, width, height}, svg->get("preserveAspectRatio"));
      viewports_.push_back({box->width, box->height});
    } else if (intrinsic) {
      const double scale = std::min(width / intrinsic->first, height / intrinsic->second);
      m = {scale, 0, 0, scale, (width - intrinsic->first * scale) / 2, (height - intrinsic->second * scale) / 2};
      viewports_.push_back({intrinsic->first, intrinsic->second});
    } else {
      viewports_.push_back({width, height});
    }
    cairo_save(cr_);
    const cairo_matrix_t matrix = m.cairo();
    cairo_transform(cr_, &matrix);
    render_children(*svg, style);
    cairo_restore(cr_);
  }

 private:
  Node* document() const {
    for (const auto& child : root_.children)
      if (child->name == "svg") return child.get();
    return nullptr;
  }

  void index(Node& node) {
    if (const std::string* id = node.attribute("id"); id != nullptr && !ids_.contains(*id)) ids_[*id] = &node;
    if (node.name == "style") parse_stylesheet(node.text, rules_);
    for (auto& child : node.children) index(*child);
  }

  Node* find(std::string_view id) const {
    const auto found = ids_.find(std::string(id));
    return found == ids_.end() ? nullptr : found->second;
  }

  static Node* reference(const Node& node, const std::unordered_map<std::string, Node*>& ids) {
    std::string_view href = trim(node.get("href"));
    if (!href.starts_with("#")) return nullptr;
    const auto found = ids.find(std::string(href.substr(1)));
    return found == ids.end() ? nullptr : found->second;
  }

  Style computed(const Node& node, const Style& parent) const {
    Style style = parent;
    style.opacity = 1;
    style.displayed = true;
    style.clip_path.clear();
    style.mask.clear();
    style.filter.clear();
    style.stop_color = {0, 0, 0, 1};
    style.stop_opacity = 1;
    for (const auto& [name, value] : node.attributes)
      if (std::ranges::find(presentation_attributes, name) != presentation_attributes.end())
        apply_property(style, parent, name, value);
    std::vector<const Rule*> matched;
    for (const Rule& rule : rules_)
      if (matches(rule, node)) matched.push_back(&rule);
    std::ranges::stable_sort(matched, [](const Rule* a, const Rule* b) {
      return a->specificity != b->specificity ? a->specificity < b->specificity : a->order < b->order;
    });
    for (const Rule* rule : matched)
      for (const auto& [name, value] : rule->declarations) apply_property(style, parent, name, value);
    for (const auto& [name, value] : parse_declarations(node.get("style"))) apply_property(style, parent, name, value);
    return style;
  }

  static double to_pixels(const Length& length, double font_size) {
    switch (length.unit) {
      case Unit::pt: return length.value * 4.0 / 3.0;
      case Unit::pc: return length.value * 16;
      case Unit::mm: return length.value * 96 / 25.4;
      case Unit::cm: return length.value * 96 / 2.54;
      case Unit::in: return length.value * 96;
      case Unit::em: return length.value * font_size;
      case Unit::ex: return length.value * font_size / 2;
      default: return length.value;
    }
  }

  enum class Axis { x, y, other };

  double resolve(const Length& length, Axis axis, const Style& style) const {
    if (length.unit != Unit::percent) return to_pixels(length, style.font_size);
    const Viewport view = viewports_.empty() ? Viewport{} : viewports_.back();
    const double reference = axis == Axis::x ? view.width : axis == Axis::y ? view.height
        : std::sqrt((view.width * view.width + view.height * view.height) / 2);
    return length.value / 100 * reference;
  }

  double length(const Node& node, std::string_view name, Axis axis, const Style& style, double fallback = 0) const {
    const auto parsed = parse_length(node.get(name));
    return parsed ? resolve(*parsed, axis, style) : fallback;
  }

  static std::optional<Bounds> view_box(const Node& node) {
    Scanner scanner(node.get("viewBox"));
    std::array<double, 4> v{};
    for (double& value : v) {
      const auto number = scanner.number();
      if (!number) return std::nullopt;
      value = *number;
    }
    if (v[2] <= 0 || v[3] <= 0) return std::nullopt;
    return Bounds{v[0], v[1], v[2], v[3]};
  }

  // preserveAspectRatio: the transform putting `box` into `port`.
  static Matrix fit(const Bounds& box, const Bounds& port, std::string_view aspect) {
    const auto words = split_words(aspect);
    std::string_view align = words.empty() ? "xMidYMid" : words[0];
    if (align == "defer" && words.size() > 1) align = words[1];
    const bool slice = std::ranges::find(words, std::string_view("slice")) != words.end();
    double sx = port.width / box.width, sy = port.height / box.height;
    if (align != "none") {
      const double scale = slice ? std::max(sx, sy) : std::min(sx, sy);
      sx = sy = scale;
    }
    double tx = port.x - box.x * sx, ty = port.y - box.y * sy;
    const double extra_x = port.width - box.width * sx, extra_y = port.height - box.height * sy;
    if (align.find("xMid") != std::string_view::npos) tx += extra_x / 2;
    else if (align.find("xMax") != std::string_view::npos) tx += extra_x;
    if (align.find("YMid") != std::string_view::npos) ty += extra_y / 2;
    else if (align.find("YMax") != std::string_view::npos) ty += extra_y;
    return {sx, 0, 0, sy, tx, ty};
  }

  void render_children(const Node& node, const Style& style) {
    for (const auto& child : node.children) render_node(*child, style);
  }

  void render_node(const Node& node, const Style& parent) {
    if (depth_ > 64) return;
    const std::string_view name = node.name;
    if (name == "defs" || name == "symbol" || name == "linearGradient" || name == "radialGradient" ||
        name == "clipPath" || name == "mask" || name == "pattern" || name == "marker" || name == "style" ||
        name == "title" || name == "desc" || name == "metadata" || name == "filter" || name == "script")
      return;
    const Style style = computed(node, parent);
    if (!style.displayed) return;
    ++depth_;
    cairo_save(cr_);
    if (const std::string* transform = node.attribute("transform")) {
      const cairo_matrix_t matrix = parse_transform(*transform).cairo();
      cairo_transform(cr_, &matrix);
    }
    const bool layered = style.opacity < 1 || !style.clip_path.empty() || !style.mask.empty() || !style.filter.empty();
    if (layered) cairo_push_group(cr_);
    draw(node, style);
    if (layered) finish_layer(node, style);
    cairo_restore(cr_);
    --depth_;
  }

  void draw(const Node& node, const Style& style) {
    const std::string_view name = node.name;
    if (name == "g" || name == "a") {
      render_children(node, style);
    } else if (name == "svg") {
      nested_svg(node, style);
    } else if (name == "switch") {
      for (const auto& child : node.children)
        if (!child->attribute("requiredExtensions") && !child->attribute("systemLanguage")) {
          render_node(*child, style);
          break;
        }
    } else if (name == "use") {
      use(node, style);
    } else if (name == "image") {
      image(node, style);
    } else if (name == "text") {
      text(node, style);
    } else if (build_path(node, style)) {
      paint_path(node, style);
    }
  }

  void finish_layer(const Node& node, const Style& style) {
    cairo_pattern_t* content = cairo_pop_group(cr_);
    if (!style.filter.empty()) content = filter(content, style.filter, node, style);
    cairo_pattern_t* clip = nullptr;
    if (Node* clip_node = style.clip_path.empty() ? nullptr : find(style.clip_path); clip_node != nullptr && clip_node->name == "clipPath")
      clip = clip_layer(*clip_node, node, style);
    cairo_pattern_t* mask = nullptr;
    if (Node* mask_node = style.mask.empty() ? nullptr : find(style.mask); mask_node != nullptr && mask_node->name == "mask")
      mask = mask_layer(*mask_node, node, style);
    if (mask != nullptr && clip != nullptr) {
      // Both: the clip shapes limit the mask.
      cairo_push_group(cr_);
      cairo_set_source(cr_, mask);
      cairo_mask(cr_, clip);
      cairo_pattern_destroy(mask);
      mask = cairo_pop_group(cr_);
      cairo_pattern_destroy(clip);
      clip = nullptr;
    }
    cairo_pattern_t* limit = mask != nullptr ? mask : clip;
    cairo_set_source(cr_, content);
    if (limit != nullptr) {
      if (style.opacity < 1) {
        cairo_push_group(cr_);
        cairo_set_source(cr_, content);
        cairo_mask(cr_, limit);
        cairo_pop_group_to_source(cr_);
        cairo_paint_with_alpha(cr_, style.opacity);
      } else {
        cairo_mask(cr_, limit);
      }
      cairo_pattern_destroy(limit);
    } else {
      cairo_paint_with_alpha(cr_, style.opacity);
    }
    cairo_pattern_destroy(content);
  }

  // The geometric bounding box of an element and its children, in its own
  // user space.
  Bounds bounding_box(const Node& node, const Style& style) {
    cairo_save(cr_);
    cairo_new_path(cr_);
    collect_geometry(node, style, 0);
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    cairo_path_extents(cr_, &x1, &y1, &x2, &y2);
    cairo_new_path(cr_);
    cairo_restore(cr_);
    return {x1, y1, x2 - x1, y2 - y1};
  }

  void collect_geometry(const Node& node, const Style& style, int depth) {
    if (depth > 32) return;
    if (node.name == "g" || node.name == "a" || (depth == 0 && (node.name == "svg" || node.name == "use"))) {
      if (node.name == "use") {
        if (Node* target = reference(node, ids_)) collect_geometry(*target, style, depth + 1);
        return;
      }
      for (const auto& child : node.children) {
        cairo_save(cr_);
        if (const std::string* transform = child->attribute("transform")) {
          const cairo_matrix_t matrix = parse_transform(*transform).cairo();
          cairo_transform(cr_, &matrix);
        }
        collect_geometry(*child, computed(*child, style), depth + 1);
        cairo_restore(cr_);
      }
      return;
    }
    if (node.name == "image" || node.name == "text") {
      const double x = length(node, "x", Axis::x, style), y = length(node, "y", Axis::y, style);
      const double w = length(node, "width", Axis::x, style), h = length(node, "height", Axis::y, style);
      cairo_rectangle(cr_, x, y - (node.name == "text" ? style.font_size : 0), node.name == "text" ? style.font_size : w,
                      node.name == "text" ? style.font_size : h);
      return;
    }
    build_path_into_current(node, style);
  }

  // A clip path as an opaque coverage pattern.
  cairo_pattern_t* clip_layer(const Node& clip, const Node& target, const Style& target_style) {
    const Bounds box = clip.get("clipPathUnits") == "objectBoundingBox" ? bounding_box(target, target_style) : Bounds{};
    cairo_push_group(cr_);
    cairo_save(cr_);
    if (clip.get("clipPathUnits") == "objectBoundingBox") {
      cairo_translate(cr_, box.x, box.y);
      cairo_scale(cr_, box.width, box.height);
    }
    if (const std::string* transform = clip.attribute("transform")) {
      const cairo_matrix_t matrix = parse_transform(*transform).cairo();
      cairo_transform(cr_, &matrix);
    }
    const Style clip_style = computed(clip, target_style);
    for (const auto& child : clip.children) {
      Style style = computed(*child, clip_style);
      if (!style.displayed || !style.visible) continue;
      cairo_save(cr_);
      if (const std::string* transform = child->attribute("transform")) {
        const cairo_matrix_t matrix = parse_transform(*transform).cairo();
        cairo_transform(cr_, &matrix);
      }
      const Node* shape = child.get();
      if (child->name == "use") {
        shape = reference(*child, ids_);
        cairo_translate(cr_, length(*child, "x", Axis::x, style), length(*child, "y", Axis::y, style));
        if (shape != nullptr) style = computed(*shape, style);
      }
      if (shape != nullptr) {
        cairo_new_path(cr_);
        if (shape->name == "text") {
          Style solid = style;
          solid.fill = solid_black();
          solid.stroke = {};
          text(*shape, solid);
        } else if (build_path(*shape, style)) {
          cairo_set_fill_rule(cr_, style.clip_evenodd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING);
          cairo_set_source_rgba(cr_, 0, 0, 0, 1);
          cairo_fill(cr_);
        }
      }
      cairo_restore(cr_);
    }
    cairo_restore(cr_);
    cairo_pattern_t* pattern = cairo_pop_group(cr_);
    // The clip path's own clip-path intersects it.
    if (!clip_style.clip_path.empty() && clip_style.clip_path != clip.get("id"))
      if (Node* outer = find(clip_style.clip_path); outer != nullptr && outer->name == "clipPath" && depth_ < 32) {
        ++depth_;
        cairo_pattern_t* outer_pattern = clip_layer(*outer, target, target_style);
        --depth_;
        cairo_push_group(cr_);
        cairo_set_source(cr_, pattern);
        cairo_mask(cr_, outer_pattern);
        cairo_pattern_destroy(pattern);
        cairo_pattern_destroy(outer_pattern);
        pattern = cairo_pop_group(cr_);
      }
    return pattern;
  }

  // A luminance mask as alpha.
  cairo_pattern_t* mask_layer(const Node& mask, const Node& target, const Style& target_style) {
    cairo_surface_t* reference_surface = cairo_get_target(cr_);
    const int width = cairo_image_surface_get_width(reference_surface);
    const int height = cairo_image_surface_get_height(reference_surface);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t* saved = cr_;
    cr_ = cairo_create(surface);
    cairo_matrix_t matrix;
    cairo_get_matrix(saved, &matrix);
    cairo_set_matrix(cr_, &matrix);
    const Bounds box = bounding_box(target, target_style);
    const bool user_units = mask.get("maskUnits") == "userSpaceOnUse";
    Bounds region;
    if (user_units) {
      region = {length(mask, "x", Axis::x, target_style, -0.1 * viewports_.back().width),
                length(mask, "y", Axis::y, target_style, -0.1 * viewports_.back().height),
                length(mask, "width", Axis::x, target_style, 1.2 * viewports_.back().width),
                length(mask, "height", Axis::y, target_style, 1.2 * viewports_.back().height)};
    } else {
      const auto fraction = [&](std::string_view name, double fallback) {
        const auto parsed = parse_length(mask.get(name));
        if (!parsed) return fallback;
        return parsed->unit == Unit::percent ? parsed->value / 100 : parsed->value;
      };
      region = {box.x + fraction("x", -0.1) * box.width, box.y + fraction("y", -0.1) * box.height,
                fraction("width", 1.2) * box.width, fraction("height", 1.2) * box.height};
    }
    cairo_rectangle(cr_, region.x, region.y, region.width, region.height);
    cairo_clip(cr_);
    if (mask.get("maskContentUnits") == "objectBoundingBox") {
      cairo_translate(cr_, box.x, box.y);
      cairo_scale(cr_, box.width, box.height);
    }
    const Style mask_style = computed(mask, target_style);
    ++depth_;
    render_children(mask, mask_style);
    --depth_;
    cairo_destroy(cr_);
    cr_ = saved;
    cairo_surface_flush(surface);
    cairo_surface_t* alpha = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
    const unsigned char* source = cairo_image_surface_get_data(surface);
    unsigned char* destination = cairo_image_surface_get_data(alpha);
    const int source_stride = cairo_image_surface_get_stride(surface);
    const int destination_stride = cairo_image_surface_get_stride(alpha);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        const unsigned char* p = source + y * source_stride + x * 4;
        // Premultiplied colour already carries the alpha.
        const double luminance = 0.2125 * p[2] + 0.7154 * p[1] + 0.0721 * p[0];
        destination[y * destination_stride + x] = static_cast<unsigned char>(std::clamp(luminance + 0.5, 0.0, 255.0));
      }
    cairo_surface_mark_dirty(alpha);
    cairo_surface_destroy(surface);
    cairo_pattern_t* pattern = cairo_pattern_create_for_surface(alpha);
    cairo_surface_destroy(alpha);
    // The mask was drawn in device space: user space maps to it by the CTM.
    cairo_matrix_t user_to_device;
    cairo_get_matrix(cr_, &user_to_device);
    cairo_pattern_set_matrix(pattern, &user_to_device);
    return pattern;
  }

  // feGaussianBlur and feOffset on the source graphic; other primitives
  // leave the content as it is.
  cairo_pattern_t* filter(cairo_pattern_t* content, const std::string& id, const Node&, const Style&) {
    Node* node = find(id);
    if (node == nullptr || node->name != "filter") return content;
    double sigma_x = 0, sigma_y = 0, dx = 0, dy = 0;
    for (const auto& primitive : node->children) {
      if (primitive->name == "feGaussianBlur") {
        Scanner scanner(primitive->get("stdDeviation"));
        sigma_x = scanner.number().value_or(0);
        sigma_y = scanner.number().value_or(sigma_x);
      } else if (primitive->name == "feOffset") {
        dx = Scanner(primitive->get("dx")).number().value_or(0);
        dy = Scanner(primitive->get("dy")).number().value_or(0);
      }
    }
    if (sigma_x <= 0 && sigma_y <= 0 && dx == 0 && dy == 0) return content;
    cairo_matrix_t matrix;
    cairo_get_matrix(cr_, &matrix);
    const double scale_x = std::hypot(matrix.xx, matrix.yx), scale_y = std::hypot(matrix.xy, matrix.yy);
    cairo_surface_t* source = nullptr;
    cairo_pattern_get_surface(content, &source);
    if (source == nullptr || cairo_surface_get_type(source) != CAIRO_SURFACE_TYPE_IMAGE) return content;
    cairo_surface_flush(source);
    const cairo_format_t format = cairo_image_surface_get_format(source);
    const int width = cairo_image_surface_get_width(source), height = cairo_image_surface_get_height(source);
    if (format != CAIRO_FORMAT_ARGB32 && format != CAIRO_FORMAT_RGBA128F) return content;
    const bool wide = format == CAIRO_FORMAT_RGBA128F;
    const int stride = cairo_image_surface_get_stride(source);
    unsigned char* data = cairo_image_surface_get_data(source);
    std::vector<float> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x)
        for (int c = 0; c < 4; ++c) {
          float value = 0;
          if (wide) std::memcpy(&value, data + y * stride + x * 16 + c * 4, 4);
          else value = data[y * stride + x * 4 + c] / 255.0F;
          pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4 + static_cast<std::size_t>(c)] = value;
        }
    // Three box blurs approximate a Gaussian.
    const auto blur = [&](double sigma, bool horizontal) {
      if (sigma <= 0) return;
      const int box = std::max(1, static_cast<int>(std::floor(sigma * 3 * std::sqrt(2 * M_PI) / 4 + 0.5)));
      const int lines = horizontal ? height : width, count = horizontal ? width : height;
      std::vector<float> line(static_cast<std::size_t>(count) * 4);
      for (int pass = 0; pass < 3; ++pass)
        for (int l = 0; l < lines; ++l) {
          const auto at = [&](int i) -> float* {
            const int x = horizontal ? i : l, y = horizontal ? l : i;
            return &pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4];
          };
          for (int i = 0; i < count; ++i) std::memcpy(&line[static_cast<std::size_t>(i) * 4], at(i), 16);
          const int before = box / 2, after = box - before - 1;
          std::array<float, 4> sum{};
          for (int i = -before - 1; i < after; ++i)
            if (i >= 0 && i < count)
              for (std::size_t c = 0; c < 4; ++c) sum[c] += line[static_cast<std::size_t>(i) * 4 + c];
          for (int i = 0; i < count; ++i) {
            const int add = i + after, drop = i - before - 1;
            for (std::size_t c = 0; c < 4; ++c) {
              if (add >= 0 && add < count) sum[c] += line[static_cast<std::size_t>(add) * 4 + c];
              if (drop >= 0 && drop < count) sum[c] -= line[static_cast<std::size_t>(drop) * 4 + c];
              at(i)[c] = sum[c] / static_cast<float>(box);
            }
          }
        }
    };
    blur(sigma_x * scale_x, true);
    blur(sigma_y * scale_y, false);
    const int shift_x = static_cast<int>(std::lround(dx * scale_x)), shift_y = static_cast<int>(std::lround(dy * scale_y));
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        const int sx = x - shift_x, sy = y - shift_y;
        for (int c = 0; c < 4; ++c) {
          float value = 0;
          if (sx >= 0 && sy >= 0 && sx < width && sy < height)
            value = pixels[(static_cast<std::size_t>(sy) * static_cast<std::size_t>(width) + static_cast<std::size_t>(sx)) * 4 + static_cast<std::size_t>(c)];
          if (wide) std::memcpy(data + y * stride + x * 16 + c * 4, &value, 4);
          else data[y * stride + x * 4 + c] = static_cast<unsigned char>(std::clamp(value * 255.0F + 0.5F, 0.0F, 255.0F));
        }
      }
    cairo_surface_mark_dirty(source);
    return content;
  }

  void nested_svg(const Node& node, const Style& style) {
    const double x = length(node, "x", Axis::x, style), y = length(node, "y", Axis::y, style);
    const double w = length(node, "width", Axis::x, style, viewports_.back().width);
    const double h = length(node, "height", Axis::y, style, viewports_.back().height);
    if (w <= 0 || h <= 0) return;
    cairo_rectangle(cr_, x, y, w, h);
    cairo_clip(cr_);
    const auto box = view_box(node);
    if (box) {
      const cairo_matrix_t matrix = fit(*box, {x, y, w, h}, node.get("preserveAspectRatio")).cairo();
      cairo_transform(cr_, &matrix);
      viewports_.push_back({box->width, box->height});
    } else {
      cairo_translate(cr_, x, y);
      viewports_.push_back({w, h});
    }
    render_children(node, style);
    viewports_.pop_back();
  }

  void use(const Node& node, const Style& style) {
    Node* target = reference(node, ids_);
    if (target == nullptr || using_.contains(target)) return;
    using_.insert(target);
    cairo_translate(cr_, length(node, "x", Axis::x, style), length(node, "y", Axis::y, style));
    if (target->name == "symbol") {
      const Style symbol_style = computed(*target, style);
      const double w = length(node, "width", Axis::x, style, length(*target, "width", Axis::x, style, viewports_.back().width));
      const double h = length(node, "height", Axis::y, style, length(*target, "height", Axis::y, style, viewports_.back().height));
      if (const auto box = view_box(*target)) {
        const cairo_matrix_t matrix = fit(*box, {0, 0, w, h}, target->get("preserveAspectRatio")).cairo();
        cairo_transform(cr_, &matrix);
        viewports_.push_back({box->width, box->height});
        render_children(*target, symbol_style);
        viewports_.pop_back();
      } else {
        render_children(*target, symbol_style);
      }
    } else if (target->name == "svg") {
      const Style svg_style = computed(*target, style);
      nested_svg(*target, svg_style);
    } else {
      render_node(*target, style);
    }
    using_.erase(target);
  }

  // Shapes and paths into cairo's current path; false when there is none.
  bool build_path(const Node& node, const Style& style) {
    cairo_new_path(cr_);
    return build_path_into_current(node, style);
  }

  bool build_path_into_current(const Node& node, const Style& style) {
    const std::string_view name = node.name;
    if (name == "path") {
      return path_data(node.get("d"));
    }
    if (name == "rect") {
      const double x = length(node, "x", Axis::x, style), y = length(node, "y", Axis::y, style);
      const double w = length(node, "width", Axis::x, style), h = length(node, "height", Axis::y, style);
      if (w <= 0 || h <= 0) return false;
      const auto rx_length = parse_length(node.get("rx"));
      const auto ry_length = parse_length(node.get("ry"));
      double rx = rx_length ? resolve(*rx_length, Axis::x, style) : -1;
      double ry = ry_length ? resolve(*ry_length, Axis::y, style) : -1;
      if (rx < 0) rx = ry;
      if (ry < 0) ry = rx;
      rx = std::clamp(rx, 0.0, w / 2);
      ry = std::clamp(ry, 0.0, h / 2);
      if (rx <= 0 || ry <= 0) {
        cairo_rectangle(cr_, x, y, w, h);
        return true;
      }
      const double k = 0.5522847498;
      cairo_move_to(cr_, x + rx, y);
      cairo_line_to(cr_, x + w - rx, y);
      cairo_curve_to(cr_, x + w - rx + k * rx, y, x + w, y + ry - k * ry, x + w, y + ry);
      cairo_line_to(cr_, x + w, y + h - ry);
      cairo_curve_to(cr_, x + w, y + h - ry + k * ry, x + w - rx + k * rx, y + h, x + w - rx, y + h);
      cairo_line_to(cr_, x + rx, y + h);
      cairo_curve_to(cr_, x + rx - k * rx, y + h, x, y + h - ry + k * ry, x, y + h - ry);
      cairo_line_to(cr_, x, y + ry);
      cairo_curve_to(cr_, x, y + ry - k * ry, x + rx - k * rx, y, x + rx, y);
      cairo_close_path(cr_);
      return true;
    }
    if (name == "circle" || name == "ellipse") {
      const double cx = length(node, "cx", Axis::x, style), cy = length(node, "cy", Axis::y, style);
      double rx = 0, ry = 0;
      if (name == "circle") {
        rx = ry = length(node, "r", Axis::other, style);
      } else {
        const auto rx_length = parse_length(node.get("rx"));
        const auto ry_length = parse_length(node.get("ry"));
        rx = rx_length ? resolve(*rx_length, Axis::x, style) : -1;
        ry = ry_length ? resolve(*ry_length, Axis::y, style) : -1;
        if (rx < 0) rx = ry;
        if (ry < 0) ry = rx;
      }
      if (rx <= 0 || ry <= 0) return false;
      cairo_save(cr_);
      cairo_translate(cr_, cx, cy);
      cairo_scale(cr_, rx, ry);
      cairo_move_to(cr_, 1, 0);
      cairo_arc(cr_, 0, 0, 1, 0, 2 * M_PI);
      cairo_close_path(cr_);
      cairo_restore(cr_);
      return true;
    }
    if (name == "line") {
      cairo_move_to(cr_, length(node, "x1", Axis::x, style), length(node, "y1", Axis::y, style));
      cairo_line_to(cr_, length(node, "x2", Axis::x, style), length(node, "y2", Axis::y, style));
      return true;
    }
    if (name == "polyline" || name == "polygon") {
      Scanner scanner(node.get("points"));
      bool first = true;
      for (;;) {
        const auto x = scanner.number();
        const auto y = scanner.number();
        if (!x || !y) break;
        if (first) cairo_move_to(cr_, *x, *y);
        else cairo_line_to(cr_, *x, *y);
        first = false;
      }
      if (first) return false;
      if (name == "polygon") cairo_close_path(cr_);
      return true;
    }
    return false;
  }

  void arc_to(double x0, double y0, double rx, double ry, double angle, bool large, bool sweep, double x, double y) {
    if (x0 == x && y0 == y) return;
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx == 0 || ry == 0) {
      cairo_line_to(cr_, x, y);
      return;
    }
    const double phi = angle * M_PI / 180;
    const double cos_phi = std::cos(phi), sin_phi = std::sin(phi);
    const double dx = (x0 - x) / 2, dy = (y0 - y) / 2;
    const double x1 = cos_phi * dx + sin_phi * dy, y1 = -sin_phi * dx + cos_phi * dy;
    const double lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1) {
      rx *= std::sqrt(lambda);
      ry *= std::sqrt(lambda);
    }
    const double numerator = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const double denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double factor = denominator == 0 ? 0 : std::sqrt(std::max(0.0, numerator / denominator));
    if (large == sweep) factor = -factor;
    const double cx1 = factor * rx * y1 / ry, cy1 = -factor * ry * x1 / rx;
    const double cx = cos_phi * cx1 - sin_phi * cy1 + (x0 + x) / 2;
    const double cy = sin_phi * cx1 + cos_phi * cy1 + (y0 + y) / 2;
    const auto vector_angle = [](double ux, double uy, double vx, double vy) {
      return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    };
    const double theta = vector_angle(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
    double delta = vector_angle((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
    if (!sweep && delta > 0) delta -= 2 * M_PI;
    if (sweep && delta < 0) delta += 2 * M_PI;
    cairo_save(cr_);
    cairo_translate(cr_, cx, cy);
    cairo_rotate(cr_, phi);
    cairo_scale(cr_, rx, ry);
    if (sweep) cairo_arc(cr_, 0, 0, 1, theta, theta + delta);
    else cairo_arc_negative(cr_, 0, 0, 1, theta, theta + delta);
    cairo_restore(cr_);
  }

  bool path_data(std::string_view d) {
    Scanner scanner(d);
    char command = '\0';
    double x = 0, y = 0, start_x = 0, start_y = 0;
    double control_x = 0, control_y = 0;
    char previous = '\0';
    bool any = false;
    while (!scanner.done()) {
      const char c = scanner.peek();
      if (std::isalpha(static_cast<unsigned char>(c)) != 0) {
        command = scanner.next_char();
        if (command == 'Z' || command == 'z') {
          cairo_close_path(cr_);
          x = start_x;
          y = start_y;
          previous = command;
          continue;
        }
      } else if (command == '\0') {
        return any;
      }
      const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
      const double base_x = relative ? x : 0, base_y = relative ? y : 0;
      const auto read = [&](std::size_t count) {
        std::array<double, 7> v{};
        for (std::size_t i = 0; i < count; ++i) {
          const auto n = scanner.number();
          if (!n) return std::optional<std::array<double, 7>>{};
          v[i] = *n;
        }
        return std::optional<std::array<double, 7>>{v};
      };
      const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
      if (upper == 'M') {
        const auto v = read(2);
        if (!v) return any;
        x = base_x + (*v)[0];
        y = base_y + (*v)[1];
        cairo_move_to(cr_, x, y);
        start_x = x;
        start_y = y;
        command = relative ? 'l' : 'L';
        any = true;
      } else if (upper == 'L') {
        const auto v = read(2);
        if (!v) return any;
        x = base_x + (*v)[0];
        y = base_y + (*v)[1];
        cairo_line_to(cr_, x, y);
      } else if (upper == 'H') {
        const auto v = read(1);
        if (!v) return any;
        x = base_x + (*v)[0];
        cairo_line_to(cr_, x, y);
      } else if (upper == 'V') {
        const auto v = read(1);
        if (!v) return any;
        y = base_y + (*v)[0];
        cairo_line_to(cr_, x, y);
      } else if (upper == 'C' || upper == 'S') {
        double x1 = 0, y1 = 0;
        std::optional<std::array<double, 7>> v;
        if (upper == 'C') {
          v = read(6);
          if (!v) return any;
          x1 = base_x + (*v)[0];
          y1 = base_y + (*v)[1];
          (*v)[0] = (*v)[2];
          (*v)[1] = (*v)[3];
          (*v)[2] = (*v)[4];
          (*v)[3] = (*v)[5];
        } else {
          v = read(4);
          if (!v) return any;
          const char p = static_cast<char>(std::toupper(static_cast<unsigned char>(previous)));
          x1 = p == 'C' || p == 'S' ? 2 * x - control_x : x;
          y1 = p == 'C' || p == 'S' ? 2 * y - control_y : y;
        }
        control_x = base_x + (*v)[0];
        control_y = base_y + (*v)[1];
        x = base_x + (*v)[2];
        y = base_y + (*v)[3];
        cairo_curve_to(cr_, x1, y1, control_x, control_y, x, y);
      } else if (upper == 'Q' || upper == 'T') {
        double qx = 0, qy = 0, ex = 0, ey = 0;
        if (upper == 'Q') {
          const auto v = read(4);
          if (!v) return any;
          qx = base_x + (*v)[0];
          qy = base_y + (*v)[1];
          ex = base_x + (*v)[2];
          ey = base_y + (*v)[3];
        } else {
          const auto v = read(2);
          if (!v) return any;
          const char p = static_cast<char>(std::toupper(static_cast<unsigned char>(previous)));
          qx = p == 'Q' || p == 'T' ? 2 * x - control_x : x;
          qy = p == 'Q' || p == 'T' ? 2 * y - control_y : y;
          ex = base_x + (*v)[0];
          ey = base_y + (*v)[1];
        }
        cairo_curve_to(cr_, x + 2.0 / 3 * (qx - x), y + 2.0 / 3 * (qy - y), ex + 2.0 / 3 * (qx - ex),
                       ey + 2.0 / 3 * (qy - ey), ex, ey);
        control_x = qx;
        control_y = qy;
        x = ex;
        y = ey;
      } else if (upper == 'A') {
        const auto rx = scanner.number(), ry = scanner.number(), angle = scanner.number();
        const auto large = scanner.flag(), sweep = scanner.flag();
        const auto ex = scanner.number(), ey = scanner.number();
        if (!rx || !ry || !angle || !large || !sweep || !ex || !ey) return any;
        const double end_x = base_x + *ex, end_y = base_y + *ey;
        arc_to(x, y, *rx, *ry, *angle, *large != 0, *sweep != 0, end_x, end_y);
        x = end_x;
        y = end_y;
      } else {
        return any;
      }
      previous = command;
    }
    return any;
  }

  // Sets the source for a fill or stroke; false when it paints nothing.
  bool set_paint(const Paint& paint, double opacity, const Style& style, const Bounds& box) {
    Paint::Kind kind = paint.kind;
    Color color = paint.color;
    if (kind == Paint::Kind::url) {
      Node* server = find(paint.url);
      if (server != nullptr && (server->name == "linearGradient" || server->name == "radialGradient"))
        return set_gradient(*server, opacity, style, box);
      if (server != nullptr && server->name == "solidColor") {
        const Style solid = computed(*server, style);
        color = parse_color(server->get("solid-color")).value_or(Color{0, 0, 0, 1});
        kind = Paint::Kind::color;
        (void)solid;
      } else {
        kind = paint.fallback_kind;
        color = paint.fallback;
      }
    }
    if (kind == Paint::Kind::current) {
      color = style.color;
      kind = Paint::Kind::color;
    }
    if (kind != Paint::Kind::color) return false;
    cairo_set_source_rgba(cr_, color.r, color.g, color.b, color.a * opacity);
    return true;
  }

  // Gradient attributes and stops, following href chains.
  const Node* gradient_attribute_owner(const Node& gradient, std::string_view name) const {
    const Node* node = &gradient;
    for (int hops = 0; node != nullptr && hops < 16; ++hops) {
      if (node->attribute(name) != nullptr) return node;
      node = reference(*node, ids_);
    }
    return nullptr;
  }

  std::string_view gradient_attribute(const Node& gradient, std::string_view name) const {
    const Node* owner = gradient_attribute_owner(gradient, name);
    return owner == nullptr ? std::string_view{} : owner->get(name);
  }

  bool set_gradient(const Node& gradient, double opacity, const Style& style, const Bounds& box) {
    const Node* stops_owner = &gradient;
    for (int hops = 0; stops_owner != nullptr && hops < 16; ++hops) {
      bool has_stops = false;
      for (const auto& child : stops_owner->children) has_stops = has_stops || child->name == "stop";
      if (has_stops) break;
      stops_owner = reference(*stops_owner, ids_);
    }
    const bool bounding = gradient_attribute(gradient, "gradientUnits") != "userSpaceOnUse";
    if (bounding && (box.width <= 0 || box.height <= 0)) return false;
    const auto coordinate = [&](std::string_view name, double fallback, Axis axis) {
      const auto parsed = parse_length(gradient_attribute(gradient, name));
      if (!parsed) return fallback;
      if (bounding) return parsed->unit == Unit::percent ? parsed->value / 100 : parsed->value;
      return resolve(*parsed, axis, style);
    };
    const Viewport view = viewports_.back();
    cairo_pattern_t* pattern = nullptr;
    if (gradient.name == "linearGradient") {
      pattern = cairo_pattern_create_linear(coordinate("x1", 0, Axis::x), coordinate("y1", 0, Axis::y),
                                            coordinate("x2", bounding ? 1 : view.width, Axis::x),
                                            coordinate("y2", 0, Axis::y));
    } else {
      const double cx = coordinate("cx", bounding ? 0.5 : view.width / 2, Axis::x);
      const double cy = coordinate("cy", bounding ? 0.5 : view.height / 2, Axis::y);
      const double r = coordinate("r", bounding ? 0.5 : resolve({50, Unit::percent}, Axis::other, style), Axis::other);
      const double fx = coordinate("fx", cx, Axis::x), fy = coordinate("fy", cy, Axis::y);
      const double fr = coordinate("fr", 0, Axis::other);
      pattern = cairo_pattern_create_radial(fx, fy, fr, cx, cy, r);
    }
    double last = 0;
    std::size_t count = 0;
    if (stops_owner != nullptr) {
      const Style owner_style = computed(*stops_owner, style);
      for (const auto& child : stops_owner->children) {
        if (child->name != "stop") continue;
        const Style stop = computed(*child, owner_style);
        double offset = 0;
        if (const auto parsed = parse_length(child->get("offset")))
          offset = parsed->unit == Unit::percent ? parsed->value / 100 : parsed->value;
        offset = std::clamp(std::max(offset, last), 0.0, 1.0);
        last = offset;
        cairo_pattern_add_color_stop_rgba(pattern, offset, stop.stop_color.r, stop.stop_color.g, stop.stop_color.b,
                                          stop.stop_color.a * stop.stop_opacity * opacity);
        ++count;
      }
    }
    if (count == 0) {
      cairo_pattern_destroy(pattern);
      return false;
    }
    const std::string_view spread = gradient_attribute(gradient, "spreadMethod");
    cairo_pattern_set_extend(pattern, spread == "reflect" ? CAIRO_EXTEND_REFLECT : spread == "repeat" ? CAIRO_EXTEND_REPEAT : CAIRO_EXTEND_PAD);
    Matrix m;
    if (bounding) m = {box.width, 0, 0, box.height, box.x, box.y};
    m = multiply(m, parse_transform(gradient_attribute(gradient, "gradientTransform")));
    cairo_matrix_t matrix = m.cairo();
    if (cairo_matrix_invert(&matrix) != CAIRO_STATUS_SUCCESS) {
      cairo_pattern_destroy(pattern);
      return false;
    }
    cairo_pattern_set_matrix(pattern, &matrix);
    cairo_set_source(cr_, pattern);
    cairo_pattern_destroy(pattern);
    return true;
  }

  void paint_path(const Node&, const Style& style) {
    if (!style.visible) {
      cairo_new_path(cr_);
      return;
    }
    cairo_path_t* path = cairo_copy_path(cr_);
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    cairo_path_extents(cr_, &x1, &y1, &x2, &y2);
    const Bounds box{x1, y1, x2 - x1, y2 - y1};
    if (set_paint(style.fill, style.fill_opacity, style, box)) {
      cairo_set_fill_rule(cr_, style.fill_evenodd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING);
      cairo_fill_preserve(cr_);
    }
    const double width = resolve(style.stroke_width, Axis::other, style);
    if (width > 0 && set_paint(style.stroke, style.stroke_opacity, style, box)) {
      cairo_new_path(cr_);
      cairo_append_path(cr_, path);
      cairo_set_line_width(cr_, width);
      cairo_set_line_cap(cr_, style.line_cap);
      cairo_set_line_join(cr_, style.line_join);
      cairo_set_miter_limit(cr_, style.miter_limit);
      std::vector<double> dashes;
      double total = 0;
      for (const Length& dash : style.dashes) {
        dashes.push_back(std::max(0.0, resolve(dash, Axis::other, style)));
        total += dashes.back();
      }
      if (dashes.size() % 2 == 1) dashes.insert(dashes.end(), dashes.begin(), dashes.end());
      if (total > 0)
        cairo_set_dash(cr_, dashes.data(), static_cast<int>(dashes.size()), resolve(style.dash_offset, Axis::other, style));
      cairo_stroke(cr_);
    }
    cairo_new_path(cr_);
    cairo_path_destroy(path);
  }

  void image(const Node& node, const Style& style) {
    if (!style.visible) return;
    const std::string_view href = trim(node.get("href"));
    std::vector<std::uint8_t> bytes;
    if (href.starts_with("data:")) {
      const std::size_t comma = href.find(',');
      if (comma == std::string_view::npos || href.substr(0, comma).find(";base64") == std::string_view::npos) return;
      bytes = base64(href.substr(comma + 1));
    } else if (!href.empty()) {
      std::filesystem::path path(href.starts_with("file://") ? href.substr(7) : href);
      if (path.is_relative()) path = base_ / path;
      std::ifstream file(path, std::ios::binary);
      if (!file) return;
      bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    if (bytes.empty()) return;
    DecodedRaster raster;
    try {
      raster = bytes.size() > 4 && (std::memcmp(bytes.data(), "<svg", 4) == 0 || std::memcmp(bytes.data(), "<?xm", 4) == 0)
          ? render_svg(bytes, 0, 0, base_)
          : decode_raster(bytes);
    } catch (const std::exception&) {
      return;
    }
    const double x = length(node, "x", Axis::x, style), y = length(node, "y", Axis::y, style);
    const double w = length(node, "width", Axis::x, style, raster.width);
    const double h = length(node, "height", Axis::y, style, raster.height);
    if (w <= 0 || h <= 0) return;
    cairo_surface_t* surface = to_surface(raster);
    cairo_save(cr_);
    cairo_rectangle(cr_, x, y, w, h);
    cairo_clip(cr_);
    const cairo_matrix_t matrix =
        fit({0, 0, static_cast<double>(raster.width), static_cast<double>(raster.height)}, {x, y, w, h},
            node.get("preserveAspectRatio"))
            .cairo();
    cairo_transform(cr_, &matrix);
    cairo_set_source_surface(cr_, surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr_), CAIRO_FILTER_GOOD);
    cairo_paint(cr_);
    cairo_restore(cr_);
    cairo_surface_destroy(surface);
  }

  static cairo_surface_t* to_surface(const DecodedRaster& raster) {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, raster.width, raster.height);
    unsigned char* data = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < raster.height; ++y)
      for (int x = 0; x < raster.width; ++x) {
        const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(raster.width) + static_cast<std::size_t>(x);
        unsigned r = 0, g = 0, b = 0, a = 0;
        if (raster.deep()) {
          const std::uint64_t p = raster.rgba16[i];
          r = static_cast<unsigned>((p >> 8U) & 255U);
          g = static_cast<unsigned>((p >> 24U) & 255U);
          b = static_cast<unsigned>((p >> 40U) & 255U);
          a = static_cast<unsigned>((p >> 56U) & 255U);
        } else {
          const std::uint32_t p = raster.rgba[i];
          r = p & 255U;
          g = (p >> 8U) & 255U;
          b = (p >> 16U) & 255U;
          a = p >> 24U;
        }
        const auto premultiply = [a](unsigned v) { return static_cast<unsigned char>((v * a + 127U) / 255U); };
        unsigned char* out = data + y * stride + x * 4;
        out[0] = premultiply(b);
        out[1] = premultiply(g);
        out[2] = premultiply(r);
        out[3] = static_cast<unsigned char>(a);
      }
    cairo_surface_mark_dirty(surface);
    return surface;
  }

  static std::vector<std::uint8_t> base64(std::string_view text) {
    std::vector<std::uint8_t> out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : text) {
      int value = -1;
      if (c >= 'A' && c <= 'Z') value = c - 'A';
      else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
      else if (c >= '0' && c <= '9') value = c - '0' + 52;
      else if (c == '+' || c == '-') value = 62;
      else if (c == '/' || c == '_') value = 63;
      else if (c == '=') break;
      if (value < 0) continue;
      buffer = buffer << 6U | static_cast<std::uint32_t>(value);
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out.push_back(static_cast<std::uint8_t>(buffer >> bits));
      }
    }
    return out;
  }

  // Text through vw's text renderer: each run is laid out on one line and
  // composited at the glyphs' device size.
  void text(const Node& node, const Style& style) {
    double x = length(node, "x", Axis::x, style), y = length(node, "y", Axis::y, style);
    x += length(node, "dx", Axis::x, style);
    y += length(node, "dy", Axis::y, style);
    struct Run {
      std::string text;
      Style style;
      std::optional<double> x, y;
      double dx = 0, dy = 0;
    };
    std::vector<Run> runs;
    std::function<void(const Node&, const Style&)> collect = [&](const Node& element, const Style& element_style) {
      std::size_t position = 0;
      const auto add_text = [&](std::string_view piece) {
        std::string collapsed;
        for (const char c : piece) {
          const char normalized = c == '\n' || c == '\t' || c == '\r' ? ' ' : c;
          if (normalized == ' ' && !collapsed.empty() && collapsed.back() == ' ') continue;
          collapsed.push_back(normalized);
        }
        if (collapsed.empty()) return;
        runs.push_back({collapsed, element_style, {}, {}, 0, 0});
      };
      for (const auto& [offset, child] : element.text_children) {
        add_text(std::string_view(element.text).substr(position, offset - position));
        position = offset;
        if (child->name != "tspan" && child->name != "a") continue;
        const Style child_style = computed(*child, element_style);
        if (!child_style.displayed) continue;
        const std::size_t first = runs.size();
        collect(*child, child_style);
        if (first < runs.size()) {
          if (const auto px = parse_length(child->get("x"))) runs[first].x = resolve(*px, Axis::x, child_style);
          if (const auto py = parse_length(child->get("y"))) runs[first].y = resolve(*py, Axis::y, child_style);
          runs[first].dx = length(*child, "dx", Axis::x, child_style);
          runs[first].dy = length(*child, "dy", Axis::y, child_style);
        }
      }
      add_text(std::string_view(element.text).substr(position));
    };
    collect(node, style);
    if (!runs.empty()) {
      std::string& first = runs.front().text;
      if (!first.empty() && first.front() == ' ') first.erase(0, 1);
      std::string& last = runs.back().text;
      if (!last.empty() && last.back() == ' ') last.pop_back();
    }
    cairo_matrix_t ctm;
    cairo_get_matrix(cr_, &ctm);
    const double scale = std::sqrt(std::abs(ctm.xx * ctm.yy - ctm.xy * ctm.yx));
    if (scale <= 0) return;
    // Anchors shift whole chunks, which start at each absolutely placed run.
    std::size_t chunk_start = 0;
    std::vector<double> advances(runs.size(), 0);
    std::vector<TextImage> images(runs.size());
    for (std::size_t i = 0; i < runs.size(); ++i) {
      const Run& run = runs[i];
      if (run.text.empty() || run.style.font_size <= 0) continue;
      const Color fill = run.style.fill.kind == Paint::Kind::current ? run.style.color : run.style.fill.color;
      const std::array<float, 4> color{static_cast<float>(fill.r), static_cast<float>(fill.g), static_cast<float>(fill.b), 1.0F};
      images[i] = rasterize_text(run.text, run.style.font_family, static_cast<float>(run.style.font_size * scale),
                                 FcWeightFromOpenType(std::clamp(run.style.font_weight, 1, 1000)), color);
      advances[i] = images[i].advance / scale;
    }
    const auto place_chunk = [&](std::size_t from, std::size_t to, double start_x, double baseline) {
      double width = 0;
      for (std::size_t i = from; i < to; ++i) width += advances[i] + (i > from ? runs[i].dx : 0);
      const std::string& anchor = runs[from].style.text_anchor;
      double pen = start_x - (anchor == "middle" ? width / 2 : anchor == "end" ? width : 0);
      double pen_y = baseline;
      for (std::size_t i = from; i < to; ++i) {
        if (i > from) {
          pen += runs[i].dx;
          pen_y += runs[i].dy;
        }
        draw_text(images[i], runs[i].style, pen, pen_y, scale);
        pen += advances[i];
      }
      return std::pair{pen, pen_y};
    };
    double pen_x = x, pen_y = y;
    for (std::size_t i = 0; i <= runs.size(); ++i) {
      const bool boundary = i == runs.size() || (i > chunk_start && (runs[i].x || runs[i].y));
      if (!boundary) continue;
      if (chunk_start < i) {
        double start_x = runs[chunk_start].x.value_or(pen_x) + runs[chunk_start].dx;
        double start_y = runs[chunk_start].y.value_or(pen_y) + runs[chunk_start].dy;
        std::tie(pen_x, pen_y) = place_chunk(chunk_start, i, start_x, start_y);
      }
      chunk_start = i;
    }
  }

  void draw_text(const TextImage& image, const Style& style, double x, double y, double scale) {
    if (image.width == 0 || !style.visible || !image.pixels) return;
    const double unit = 1.0 / (scale * image.scale);
    cairo_save(cr_);
    if (image.alpha_mask) {
      // The paint is set in the text's user space, then the coverage is
      // applied in the bitmap's.
      const Bounds box{x, y - style.font_size, image.advance / scale, style.font_size * 1.2};
      if (!set_paint(style.fill, style.fill_opacity, style, box)) {
        cairo_restore(cr_);
        return;
      }
      cairo_surface_t* coverage =
          cairo_image_surface_create(CAIRO_FORMAT_A8, static_cast<int>(image.width), static_cast<int>(image.height));
      unsigned char* data = cairo_image_surface_get_data(coverage);
      const auto stride = static_cast<std::size_t>(cairo_image_surface_get_stride(coverage));
      for (std::uint32_t row = 0; row < image.height; ++row)
        for (std::uint32_t column = 0; column < image.width; ++column) {
          const std::size_t index = static_cast<std::size_t>(row) * image.width + column;
          data[row * stride + column] = static_cast<unsigned char>(((*image.pixels)[index / 4U] >> ((index & 3U) * 8U)) & 255U);
        }
      cairo_surface_mark_dirty(coverage);
      cairo_translate(cr_, x, y);
      cairo_scale(cr_, unit, unit);
      cairo_mask_surface(cr_, coverage, -image.origin_x, -image.baseline_y);
      cairo_surface_destroy(coverage);
    } else if (style.fill.kind != Paint::Kind::none) {
      DecodedRaster color;
      color.width = static_cast<int>(image.width);
      color.height = static_cast<int>(image.height);
      color.rgba = *image.pixels;
      cairo_surface_t* surface = to_surface(color);
      cairo_translate(cr_, x, y);
      cairo_scale(cr_, unit, unit);
      cairo_set_source_surface(cr_, surface, -image.origin_x, -image.baseline_y);
      cairo_paint_with_alpha(cr_, style.fill_opacity);
      cairo_surface_destroy(surface);
    }
    cairo_restore(cr_);
  }

  Node& root_;
  std::filesystem::path base_;
  cairo_t* cr_ = nullptr;
  std::unordered_map<std::string, Node*> ids_;
  std::vector<Rule> rules_;
  std::vector<Viewport> viewports_;
  std::set<const Node*> using_;
  int depth_ = 0;
};

}  // namespace

std::optional<std::array<double, 4>> parse_css_color(std::string_view text) {
  const auto color = parse_color(text);
  if (!color) return std::nullopt;
  return std::array<double, 4>{color->r, color->g, color->b, color->a};
}

std::optional<std::pair<double, double>> svg_intrinsic_size(std::span<const std::uint8_t> bytes) {
  std::vector<std::uint8_t> inflated;
  if (bytes.size() >= 2 && bytes[0] == 0x1F && bytes[1] == 0x8B) {
    inflated = gzip_decompress(bytes, 64U << 20U);
    bytes = inflated;
  }
  std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  const auto root = XmlParser(text).parse();
  Renderer renderer(*root, {});
  return renderer.intrinsic_size();
}

DecodedRaster render_svg(std::span<const std::uint8_t> bytes, int target_width, int target_height,
                         const std::filesystem::path& base) {
  std::vector<std::uint8_t> inflated;
  if (bytes.size() >= 2 && bytes[0] == 0x1F && bytes[1] == 0x8B) {
    inflated = gzip_decompress(bytes, 64U << 20U);
    bytes = inflated;
  }
  std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  const std::unique_ptr<Node> root = XmlParser(text).parse();
  Renderer renderer(*root, base);
  const auto intrinsic = renderer.intrinsic_size();
  const double intrinsic_width = intrinsic && intrinsic->first > 0 ? intrinsic->first : 64;
  const double intrinsic_height = intrinsic && intrinsic->second > 0 ? intrinsic->second : 64;
  const int width = target_width > 0 ? target_width
      : static_cast<int>(std::ceil(target_height > 0 ? intrinsic_width * target_height / intrinsic_height : intrinsic_width));
  const int height = target_height > 0 ? target_height
      : static_cast<int>(std::ceil(target_width > 0 ? intrinsic_height * target_width / intrinsic_width : intrinsic_height));
  if (width < 1 || height < 1 || width > 8192 || height > 8192)
    throw std::runtime_error("SVG dimensions must be between 1 and 8192 pixels");

  // Float pixels keep gradients smooth past eight bits.
  cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_RGBA128F, width, height);
  bool wide = cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS;
  if (!wide) {
    cairo_surface_destroy(surface);
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  }
  cairo_t* cr = cairo_create(surface);
  try {
    renderer.render(cr, width, height);
  } catch (...) {
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    throw;
  }
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  DecodedRaster image = blank_raster(static_cast<std::uint64_t>(width), static_cast<std::uint64_t>(height));
  const unsigned char* data = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  if (wide) {
    image.rgba16.assign(image.rgba.size(), 0);
    image.rgba = {};
  }
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
      if (wide) {
        std::array<float, 4> p{};
        std::memcpy(p.data(), data + y * stride + x * 16, 16);
        const float a = std::clamp(p[3], 0.0F, 1.0F);
        const auto channel = [a](float v) {
          const float straight = a > 0 ? v / a : 0.0F;
          return static_cast<std::uint64_t>(std::clamp(straight, 0.0F, 1.0F) * 65535.0F + 0.5F);
        };
        image.rgba16[i] = pack_rgba16(channel(p[0]), channel(p[1]), channel(p[2]),
                                      static_cast<std::uint64_t>(a * 65535.0F + 0.5F));
      } else {
        const unsigned char* pixel = data + y * stride + x * 4;
        const unsigned alpha = pixel[3];
        const auto unpremultiply = [alpha](unsigned value) -> unsigned {
          return alpha == 0 ? 0 : std::min(255U, (value * 255U + alpha / 2U) / alpha);
        };
        image.rgba[i] = pack_rgba(unpremultiply(pixel[2]), unpremultiply(pixel[1]), unpremultiply(pixel[0]), alpha);
      }
    }
  cairo_surface_destroy(surface);
  narrow_if_exact(image);
  return image;
}

}  // namespace vw::detail
