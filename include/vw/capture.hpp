#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "vw/scene.hpp"

// Screen capture through wlr-screencopy, on a connection of its own. Draw the
// result with ui.image({.bitmap = ...}); fit and size it like any image.
namespace vw {

// A rectangle in an output's logical coordinates.
struct CaptureRegion {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct CaptureProps {
  // Output name such as "HDMI-A-1"; empty takes the first output.
  std::string_view output;
  // Part of the output; the whole output when unset.
  std::optional<CaptureRegion> region;
  bool cursor = false;
};

// What the output shows now. Waits for the compositor (usually one frame),
// so call it on a refresh interval rather than every build. Throws when the
// compositor has no screencopy, the output is unknown, or the capture fails.
Bitmap capture_output(const CaptureProps& props = {});

// Window capture, for overviews and switchers. zwwm offers it to clients
// that present its capture token ($ZWWM_CAPTURE_TOKEN, readable only by the
// session's user); vw does that by itself. A window is captured at its own
// size without decorations, even on a hidden tag.
struct CaptureWindow {
  // vw's id for the window, valid until it closes.
  std::uint64_t id = 0;
  std::string title;
  std::string app_id;
  // Decorated logical rectangle on `output`.
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  std::string output;
  bool activated = false;
  bool minimized = false;
  bool maximized = false;
  bool fullscreen = false;
};

// Whether zwwm granted window capture.
bool window_capture_available();
// The windows zwwm reports, in its order; empty without window capture.
std::vector<CaptureWindow> capture_windows();
// Throws when the window closed or the capture fails.
Bitmap capture_window(std::uint64_t id, bool cursor = false);

// Readable when the window list or a window's title, size or state changes;
// see vw::App::on_event. -1 without window capture.
int capture_event_fd();
bool drain_capture_events();

}  // namespace vw
