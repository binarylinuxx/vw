#pragma once

#include <optional>
#include <string_view>

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

}  // namespace vw
