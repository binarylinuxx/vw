#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vw/scene.hpp"

// Immediate-mode scene construction. A build function creates the whole scene
// from scratch through a Ui every refresh; state that must survive refreshes
// (text field contents, scroll offsets, tweens) is keyed by id.
//
//   vw::App app([](vw::Ui& ui) {
//     ui.rectangle({.width = 200, .height = 40, .radius = 12,
//                   .place = {.x = 8, .y = 8},
//                   .input = {.on_click = [](const vw::ClickEvent&) { ... }}});
//   });
namespace vw {

using Color = std::array<float, 4>;
// top, right, bottom, left
using Margin = std::array<std::int32_t, 4>;

// 0xRRGGBBAA
constexpr Color rgba(std::uint32_t value) {
  return {static_cast<float>((value >> 24) & 0xffU) / 255.0F,
          static_cast<float>((value >> 16) & 0xffU) / 255.0F,
          static_cast<float>((value >> 8) & 0xffU) / 255.0F,
          static_cast<float>(value & 0xffU) / 255.0F};
}
// 0xRRGGBB, opaque
constexpr Color rgb(std::uint32_t value) { return rgba((value << 8) | 0xffU); }

// Layer-shell anchor bits.
namespace anchor {
inline constexpr std::uint32_t center = 0;
inline constexpr std::uint32_t top = 1;
inline constexpr std::uint32_t bottom = 2;
inline constexpr std::uint32_t left = 4;
inline constexpr std::uint32_t right = 8;
inline constexpr std::uint32_t top_left = top | left;
inline constexpr std::uint32_t top_right = top | right;
inline constexpr std::uint32_t bottom_left = bottom | left;
inline constexpr std::uint32_t bottom_right = bottom | right;
inline constexpr std::uint32_t fill = top | bottom | left | right;
}  // namespace anchor

enum class Layer : std::uint32_t { background, bottom, top, overlay };
enum class Gradient : std::uint32_t { none, horizontal, vertical };

// fontconfig weights
namespace weight {
inline constexpr int thin = 0;
inline constexpr int light = 50;
inline constexpr int regular = 80;
inline constexpr int medium = 100;
inline constexpr int semibold = 180;
inline constexpr int bold = 200;
inline constexpr int black = 210;
}  // namespace weight

struct Clip {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
  float radius = 0.0F;
};

// Where a widget goes. Setting x or y without an anchor anchors the surface
// top-left for rectangles, text, images and rows/columns/stacks.
struct Placement {
  std::optional<float> x;
  std::optional<float> y;
  std::optional<std::uint32_t> anchor;
  Layer layer = Layer::top;
  Margin margin{};
  bool exclusive = false;
  // Non-zero reserves this many pixels instead of the surface height.
  std::int32_t exclusive_zone = 0;
  bool visible = true;
  std::optional<Clip> clip;
};

struct Hover {
  std::optional<Color> color;
  float scale = 1.0F;
  float duration = 0.16F;
};

struct Input {
  ClickHandler on_click;
  std::optional<Hover> hover;
  DragHandler on_drag_start;
  DragHandler on_drag;
  DragHandler on_drag_end;
  // Mouse wheel or touchpad scroll over the widget.
  ScrollHandler on_scroll;
  // The pointer entered or left the widget's rectangle, whatever is drawn on
  // top of it (like QML's HoverHandler). The scene rebuilds afterwards, so
  // handlers can open and close hover popouts by changing state.
  HoverHandler on_hover;
  std::uint32_t drag_button = 272;  // BTN_LEFT
};

// A built widget: a range of scene primitives plus its measured size. Layouts
// take widgets after they are built and move them into place.
struct Widget {
  std::size_t begin = 0;
  std::size_t end = 0;
  float width = 0.0F;
  float height = 0.0F;
  Margin margin{};
  std::uint32_t anchor = 0;
};

struct RectangleProps {
  float width = 320.0F;
  float height = 48.0F;
  float radius = 0.0F;
  Color color{0.12F, 0.14F, 0.18F, 1.0F};
  // Second gradient stop; defaults to color.
  std::optional<Color> color2;
  Gradient gradient = Gradient::none;
  float border_width = 0.0F;
  Color border_color{1.0F, 1.0F, 1.0F, 1.0F};
  // top-left, top-right, bottom-right, bottom-left; unset inherits radius.
  std::array<std::optional<float>, 4> corner_radii{};
  // Path to a SPIR-V compute shader.
  std::string shader;
  std::array<float, 4> shader_params{};
  // More data than shader_params holds (point lists, curves): the shader
  // reads it as 32-bit words from the storage buffer at binding 1
  // (`readonly buffer { uint words[]; }`, uintBitsToFloat), starting at word
  // clip_params.y, clip_params.z floats long.
  std::vector<float> shader_data;
  Placement place;
  Input input;
};

struct HitAreaProps {
  float width = 0.0F;
  float height = 0.0F;
  Placement place;
  Input input;
};

struct TextProps {
  std::string text;
  std::string font = "monospace";
  float size = 16.0F;
  int weight = weight::regular;
  Color color{0.12F, 0.14F, 0.18F, 1.0F};
  float max_width = 0.0F;
  int max_lines = 0;
  std::string align = "start";      // start, center, end
  std::string ellipsize = "end";    // none, start, middle, end
  float radius = 0.0F;
  Placement place;
  Input input;
};

struct ImageProps {
  // A file path, an icon name/serialized GIcon, or pixels in memory (such as
  // vw::capture_output) when both are empty.
  std::string path;
  std::string icon;
  Bitmap bitmap;
  // Zero keeps the image's natural size or aspect ratio.
  float width = 0.0F;
  float height = 0.0F;
  // Applies when both width and height are set.
  ImageFit fit = ImageFit::stretch;
  float radius = 0.0F;
  Color tint{1.0F, 1.0F, 1.0F, 1.0F};
  ImageEffect effect;
  // Clips the image to a closed polygon, with an antialiased edge: x0, y0, x1,
  // y1, ... from 0 to 1 across the drawn image, at least three points. Flatten
  // curves first; rounded polygons need a few points per corner. It applies
  // on top of `radius`.
  std::vector<float> mask;
  Placement place;
  Input input;
};

struct CanvasProps {
  std::string id;
  // JavaScript source, or a script path read (and cached by mtime) instead.
  std::string code;
  std::string script;
  float width = 320.0F;
  float height = 200.0F;
  float radius = 0.0F;
  float opacity = 1.0F;
  bool animate = false;
  Placement place;
  Input input;
};

struct TextFieldProps {
  std::string id;
  // Initial value the first time this id is seen.
  std::string value;
  std::string placeholder;
  float width = 240.0F;
  float height = 40.0F;
  float radius = 10.0F;
  float border_width = 1.0F;
  float padding = 12.0F;
  Color color{0.055F, 0.07F, 0.1F, 1.0F};
  // Border while unfocused, and while focused (the focus ring).
  std::optional<Color> border_color;
  Color focus_color{0.35F, 0.65F, 1.0F, 1.0F};
  std::optional<Color> text_color;
  std::optional<Color> placeholder_color;
  // Defaults to focus_color.
  std::optional<Color> caret_color;
  // Selection highlight; defaults to focus_color at low opacity.
  std::optional<Color> selection_color;
  std::string font = "monospace";
  float size = 14.0F;
  int weight = weight::regular;
  bool password = false;
  // Lock screens only: Enter checks the value with PAM.
  bool authenticate = false;
  bool autofocus = false;
  std::size_t max_length = 4096;
  TextHandler on_change;
  TextHandler on_submit;
  AuthenticationHandler on_authenticated;
  Placement place;
};

// Where children go along a row or column when it is longer than they are.
enum class Justify { start, center, end, space_between, space_around, space_evenly };
// Where a child sits across a row or column, or inside a grid cell.
enum class Align { start, center, end };

struct LayoutProps {
  float gap = 0.0F;
  float padding = 0.0F;
  // Defaults to the measured size of the children.
  std::optional<float> width;
  std::optional<float> height;
  // Takes effect when width (row) or height (column) leaves room to spare.
  Justify justify = Justify::start;
  // Across the axis; a child's own place.anchor wins over it.
  Align align = Align::center;
  float radius = 0.0F;
  Placement place;
  Input input;
};

// Children fill `columns` per row, left to right. Columns are as wide as the
// widest child, or share `width` equally when it is set; each row is as tall
// as its tallest child.
struct GridProps {
  int columns = 2;
  float gap = 0.0F;
  // Between rows; defaults to gap.
  std::optional<float> row_gap;
  float padding = 0.0F;
  std::optional<float> width;
  std::optional<float> height;
  // A child inside its cell, on both axes.
  Align align = Align::center;
  float radius = 0.0F;
  Placement place;
  Input input;
};

struct StackProps {
  std::optional<float> width;
  std::optional<float> height;
  float radius = 0.0F;
  Placement place;
  Input input;
};

struct ScrollProps {
  // Keys the retained scroll offset.
  std::string id;
  float width = 0.0F;
  float height = 0.0F;
  float gap = 0.0F;
  float padding = 0.0F;
  float scroll_step = 40.0F;
  float radius = 0.0F;
  Placement place;
  Input input;
};

struct ListviewProps {
  std::string id;
  float width = 0.0F;
  float height = 0.0F;
  float item_height = 0.0F;
  float gap = 0.0F;
  float scroll_step = 40.0F;
  float radius = 0.0F;
  std::size_t count = 0;
  // Called only for rows inside the viewport; return nullopt to skip a row.
  std::function<std::optional<Widget>(std::size_t index)> render;
  // Rows of different heights: called for every row to lay the list out
  // (keep it cheap), while render still runs only for visible rows. When
  // unset every row is item_height tall.
  std::function<float(std::size_t index)> row_height;
  // When this changes, scrolls just enough to bring the row fully into view
  // (like QML's ListView.currentIndex); the wheel scrolls freely otherwise.
  std::optional<std::size_t> keep_visible;
  Placement place;
  Input input;
};

struct AnchorBox {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
};

struct AnchorChild {
  bool left = false;
  bool right = false;
  bool top = false;
  bool bottom = false;
  Margin margin{};
  // Ignored on an axis anchored to both edges.
  std::optional<float> width;
  std::optional<float> height;
  float min_width = 1.0F;
  float min_height = 1.0F;
  float max_width = 8192.0F;
  float max_height = 8192.0F;
  // Builds the child for its resolved box; it is then moved into place.
  std::function<Widget(const AnchorBox&)> render;
};

struct AnchorLayoutProps {
  float width = 0.0F;
  float height = 0.0F;
  float radius = 0.0F;
  std::vector<AnchorChild> children;
  Placement place;
  Input input;
};

struct SurfaceProps {
  // 0 fills the output along that axis: both edges are anchored and the
  // compositor picks the size; read it back with vw::surface_size().
  // Unset keeps the size of the drawn content.
  std::optional<float> width;
  std::optional<float> height;
  std::uint32_t anchor = anchor::center;
  Layer layer = Layer::top;
  Margin margin{};
  // Pixels reserved from the anchored edge; 0 reserves nothing. Lets a surface
  // grow (e.g. to show a panel) while reserving only its bar.
  std::int32_t exclusive_zone = 0;
  std::string surface_namespace = "vw";
  // The output (monitor) to show on, by name: "DP-1". Empty lets the
  // compositor pick. See vw::outputs() and App::per_output.
  std::string output;
  // {x, y, width, height}. Present but empty makes the surface click-through.
  std::optional<std::vector<std::array<std::int32_t, 4>>> input_regions;
  bool focus_grab = false;
  FocusHandler on_activated;
  FocusHandler on_dismiss;
  // The compositor closed the layer surface.
  CloseHandler on_closed;
  // Seconds between periodic rebuilds of this surface (events and input
  // still rebuild it at once). Static surfaces can set this high.
  double refresh_interval = 1.0;
};

struct ToplevelProps {
  std::optional<float> width;
  std::optional<float> height;
  double refresh_interval = 1.0;
  std::string title = "vw";
  std::string app_id = "vw";
  // Ask the compositor to draw the titlebar and borders (server) or leave
  // them to the window (client); automatic takes the compositor's choice.
  // vw::toplevel_server_decorated() says what it picked. Needs zwwm.
  Decorations decorations = Decorations::automatic;
  // The window was closed. Without a handler the whole app exits.
  CloseHandler on_close;
};

struct SessionLockProps {
  // Defaults to "swaylock" when /etc/pam.d/swaylock exists, else "login".
  std::string pam_service;
  // Optional absolute path to a PAM configuration file.
  std::string pam_conf;
  // The session was unlocked and the lock surfaces are gone; clear whatever
  // state made the build draw the lock. Without a handler the app exits.
  CloseHandler on_unlock;
};

struct LockSurfaceProps {
  std::optional<float> width;
  std::optional<float> height;
};

// Forgets a text field's retained value and focus, e.g. when a popup closes;
// the next build starts it from TextFieldProps::value again.
void reset_text_field(std::string_view id);
// The retained scroll offset of a scrollbox or listview id (0 when unseen),
// e.g. to draw a selection highlight that moves between rows.
float scroll_offset(std::string_view id);
void set_scroll_offset(std::string_view id, float offset);

class Ui {
 public:
  explicit Ui(Scene& scene) : scene_(scene) {}
  Ui(const Ui&) = delete;
  Ui& operator=(const Ui&) = delete;

  Widget rectangle(const RectangleProps& props);
  Widget hit_area(const HitAreaProps& props);
  // Clips everything `widget` drew, and its input, to `clip`.
  void clip(const Widget& widget, const Clip& clip);
  // Multiplies the opacity of everything `widget` drew, text and images too.
  void fade(const Widget& widget, float opacity);
  Widget text(const TextProps& props);
  Widget image(const ImageProps& props);
  Widget canvas(const CanvasProps& props);
  Widget text_field(const TextFieldProps& props);

  Widget row(std::span<const Widget> children, const LayoutProps& props = {});
  Widget column(std::span<const Widget> children, const LayoutProps& props = {});
  Widget stack(std::span<const Widget> children, const StackProps& props = {});
  Widget grid(std::span<const Widget> children, const GridProps& props = {});
  Widget scrollbox(std::span<const Widget> children, const ScrollProps& props);
  Widget listview(const ListviewProps& props);
  Widget anchor_layout(const AnchorLayoutProps& props);

  Widget row(std::initializer_list<Widget> children, const LayoutProps& props = {}) {
    return row(std::span<const Widget>(children.begin(), children.size()), props);
  }
  Widget column(std::initializer_list<Widget> children, const LayoutProps& props = {}) {
    return column(std::span<const Widget>(children.begin(), children.size()), props);
  }
  Widget grid(std::initializer_list<Widget> children, const GridProps& props = {}) {
    return grid(std::span<const Widget>(children.begin(), children.size()), props);
  }
  Widget stack(std::initializer_list<Widget> children, const StackProps& props = {}) {
    return stack(std::span<const Widget>(children.begin(), children.size()), props);
  }
  Widget scrollbox(std::initializer_list<Widget> children, const ScrollProps& props) {
    return scrollbox(std::span<const Widget>(children.begin(), children.size()), props);
  }

  // Keys for this surface go to handler first. The mode sets which keyboard
  // focus a layer surface requests.
  void keyboard(KeyHandler handler, Keyboard mode = Keyboard::on_demand);

  void hide(const Widget& widget);
  void move(const Widget& widget, float dx, float dy);
  // Places a built widget in the surface: centred by default, or against
  // the edges in `where` (vw::anchor bits), `margin` ({top, right, bottom,
  // left}) away from them. The surface is the configured size when there
  // is one (a fill-width bar), else everything drawn so far.
  void align(const Widget& widget, std::uint32_t where = anchor::center, const Margin& margin = {});

  void surface(const SurfaceProps& props);
  void toplevel(const ToplevelProps& props);
  void session_lock(const SessionLockProps& props = {});
  void lock_surface(const LockSurfaceProps& props = {});

  // Validates the scene and resolves its final surface size. vw::App calls
  // this after the build function returns.
  void finish();

  Scene& scene() { return scene_; }

 private:
  Widget push(std::size_t begin, float width, float height, const Rectangle& style,
              const Input& input);
  Widget linear(std::span<const Widget> children, const LayoutProps& props, bool row);
  // Clips the placed children to the group, places the group, and adds it.
  Widget close_group(std::span<const Widget> children, float width, float height,
                     const Placement& place, float radius, const Input& input);
  Widget finish_scroll(const Rectangle& style, std::size_t begin, float content_height,
                       float step, const std::shared_ptr<ScrollState>& state,
                       const Input& input);

  Scene& scene_;
};

}  // namespace vw
