-- A slider composed from existing vw widgets, not a new native slider type.
local base = require("vw.base")
local window = require("vw.toplevel")
local animation = require("vw.animation")

window.set({title = "vw — Slider", app_id = "vw-slider-demo", width = 620, height = 380})
local size = window.size()
local runtime = os.getenv("XDG_RUNTIME_DIR") or "/tmp"
local state_path = os.getenv("VW_SLIDER_STATE") or (runtime .. "/vw-slider-demo-state")
local stored = ""
local file = io.open(state_path, "r")
if file then stored = file:read("*a") or ""; file:close() end
local state = {
  value = math.max(0, math.min(100, tonumber(stored:match("^([%d.]+)")) or 45)),
  dragging = stored:match("\n([01])") == "1",
}
local function save()
  local output = io.open(state_path .. ".tmp", "w")
  if not output then return end
  output:write(string.format("%.6f\n%d\n", state.value, state.dragging and 1 or 0))
  output:close()
  os.rename(state_path .. ".tmp", state_path)
end
local value = animation.value("slider-demo-value", state.value, {
  duration = state.dragging and 0 or 0.28, easing = "spring",
  mass = 1, stiffness = 300, damping = 24, min = 0, max = 100,
})
local thumb_radius = animation.value("slider-demo-thumb", state.dragging and 15 or 12, {
  easing = "spring", stiffness = 360, damping = 25, min = 10, max = 17,
})
local C = {
  background = {0.04, 0.055, 0.075, 1}, panel = {0.075, 0.1, 0.135, 1},
  track = {0.16, 0.21, 0.265, 1}, accent = {0.51, 0.87, 0.79, 1},
  text = {0.93, 0.96, 0.99, 1}, muted = {0.52, 0.62, 0.7, 1},
}
local function text(label, x, y, font_size, color, max_width)
  return base.text({text = label, x = x, y = y, font = "sans-serif",
    size = font_size, color = color or C.text, anchor = "top_left",
    max_width = max_width or 0, max_lines = 1})
end
base.rectangle({width = size.width, height = size.height, color = C.background})
local padding = math.min(28, size.width * 0.08)
local panel_width = math.max(1, size.width - padding * 2)
local panel_y = math.min(92, size.height * 0.27)
local panel_height = math.max(1, math.min(190, size.height - panel_y - 16))
base.rectangle({x = padding, y = panel_y, width = panel_width, height = panel_height,
                radius = 22, color = C.panel})
text("A slider, built from primitives", padding, 22, 24, C.text, panel_width)
text("Drag the handle, click the track, or try a preset.", padding, 58, 13, C.muted, panel_width)

local track_x = padding + math.min(24, panel_width * 0.1)
local track_width = math.max(1, size.width - track_x * 2)
local track_y = panel_y + math.min(100, panel_height * 0.55)
local thumb_x = track_x + track_width * value / 100
text("LEVEL", track_x, panel_y + 20, 11, C.muted)
text(string.format("%.0f%%", value), math.max(track_x, size.width - track_x - 100),
     panel_y + 16, 30, C.accent, 100)
base.rectangle({x = track_x, y = track_y - 4, width = track_width, height = 8,
                radius = 4, color = C.track})
base.rectangle({x = track_x, y = track_y - 4, width = math.max(0.01, track_width * value / 100),
                height = 8, radius = 4, color = C.accent})
for index = 0, 4 do
  local x = track_x + track_width * index / 4
  base.rectangle({x = x, y = track_y + 22, width = 1, height = 5, color = C.track})
  text(tostring(index * 25), math.max(0, x - 7), track_y + 34, 10, C.muted)
end
-- Halo and thumb are visuals only. The drag area captures the entire track.
base.rectangle({x = math.max(0, thumb_x - thumb_radius - 6), y = math.max(0, track_y - thumb_radius - 6),
  width = (thumb_radius + 6) * 2, height = (thumb_radius + 6) * 2,
  radius = thumb_radius + 6, color = {C.accent[1], C.accent[2], C.accent[3], state.dragging and 0.16 or 0.06}})
base.rectangle({x = math.max(0, thumb_x - thumb_radius), y = math.max(0, track_y - thumb_radius),
  width = thumb_radius * 2, height = thumb_radius * 2, radius = thumb_radius,
  color = C.accent, border_width = 3, border_color = {0.82, 1, 0.95, 1}})

local grab_offset = 0
local function follow_pointer(event)
  state.value = math.max(0, math.min(100, (event.x - grab_offset - track_x) / track_width * 100))
  save()
end
local start_value = state.value
base.drag({x = math.max(0, track_x - 18), y = math.max(0, track_y - 24),
  width = track_width + 36, height = 48,
  on_drag_start = function(event)
    -- Grabbing the thumb preserves the point of contact; clicking elsewhere
    -- moves directly to that point on the track.
    grab_offset = math.abs(event.x - thumb_x) <= thumb_radius + 3 and event.x - thumb_x or 0
    state.dragging = true
    follow_pointer(event)
  end,
  on_drag = follow_pointer,
  on_drag_end = function(event)
    if event.cancelled then state.value = start_value
    else follow_pointer(event); state.value = math.floor(state.value + 0.5) end
    state.dragging = false
    save()
  end})

local controls_y = panel_y + panel_height + 18
if controls_y + 40 <= size.height then
  local labels = {{"Reset", 45}, {"25%", 25}, {"75%", 75}, {"100%", 100}}
  local gap = 8
  local button_width = math.min(100, math.max(1, (panel_width - gap * 3) / 4))
  for index, preset in ipairs(labels) do
    local x = padding + (index - 1) * (button_width + gap)
    base.rectangle({x = x, y = controls_y, width = button_width, height = 36, radius = 12,
      color = C.panel, hover = {color = C.track, duration = 0.13},
      on_click = function() state.value = preset[2]; state.dragging = false; save() end})
    text(preset[1], x + 14, controls_y + 9, 12, C.text, math.max(1, button_width - 20))
  end
end
