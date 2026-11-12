-- Material-style brightness slider, composed from existing vw components.
local base = require("vw.base")
local window = require("vw.toplevel")
local animation = require("vw.animation")

window.set({title = "vw — Material Slider", app_id = "vw-material-slider", width = 640, height = 220})
local size = window.size()
local path = (os.getenv("XDG_RUNTIME_DIR") or "/tmp") .. "/vw-material-slider-state"
local contents = ""
local file = io.open(path, "r")
if file then contents = file:read("*a") or ""; file:close() end
local state = {value = math.max(0, math.min(100, tonumber(contents:match("^([%d.]+)")) or 68)),
               dragging = contents:match("\n([01])") == "1"}
local function save()
  local output = io.open(path .. ".tmp", "w")
  if not output then return end
  output:write(string.format("%.5f\n%d\n", state.value, state.dragging and 1 or 0))
  output:close()
  os.rename(path .. ".tmp", path)
end
local value = animation.value("material-slider-value", state.value, {
  easing = "spring", mass = 1, stiffness = 420, damping = 30,
  duration = state.dragging and 0 or 0.3, min = 0, max = 100,
})
local emphasis = animation.value("material-slider-press", state.dragging and 1 or 0,
  {initial = 0, duration = 0.14, easing = "out_cubic", min = 0, max = 1})
local padding = math.min(70, size.width * 0.11)
local track_width = math.max(1, size.width - padding * 2)
local height = math.max(1, math.min(74, size.height * 0.36, track_width))
local radius = height / 2
local center_y = size.height / 2
local travel = math.max(0, track_width - height)
local thumb_x = padding + radius + travel * value / 100
local active_width = height + travel * value / 100
local x_min = padding + radius

base.rectangle({width = size.width, height = size.height, color = {0.12, 0.12, 0.12, 1}})
-- Draw the thin inactive rail first; the red fill hides its left portion.
base.rectangle({x = padding + radius, y = center_y - math.min(5, height / 4),
  width = math.max(1, track_width - radius), height = math.max(1, math.min(10, height / 2)),
  radius = 5, color = {0, 0, 0, 1}})
base.rectangle({x = padding, y = center_y - radius, width = active_width, height = height,
  radius = radius, color = {0.98, 0.17, 0.18, 1}})

-- Material ligature in a centered slot avoids font-specific side bearings.
local icon_size = math.min(43 + emphasis * 2, height * 0.64)
base.stack({x = math.max(0, thumb_x - radius), y = math.max(0, center_y - radius),
  width = height, height = height, children = {
    base.text({text = "brightness_7", font = "Material Symbols Rounded", size = icon_size,
               color = {0.1, 0.1, 0.1, 1}, anchor = "center"}),
  }})

local grab_offset = 0
local before_drag = state.value
local function update(event)
  state.value = travel > 0 and math.max(0, math.min(100, (event.x - grab_offset - x_min) / travel * 100)) or 0
  save()
end
base.drag({x = padding, y = math.max(0, center_y - radius - 10), width = track_width,
  height = math.min(size.height, height + 20),
  on_drag_start = function(event)
    grab_offset = math.abs(event.x - thumb_x) < radius and event.x - thumb_x or 0
    state.dragging = true; update(event)
  end,
  on_drag = update,
  on_drag_end = function(event)
    if event.cancelled then state.value = before_drag
    else update(event); state.value = math.floor(state.value + 0.5) end
    state.dragging = false; save()
  end})
-- Small readout appears only during interaction, preserving the reference at rest.
if emphasis > 0.01 then
  local bubble_width = math.min(52, size.width)
  local bubble_x = math.max(0, math.min(size.width - bubble_width, thumb_x - bubble_width / 2))
  local bubble_y = math.max(0, center_y - radius - 39)
  base.rectangle({x = bubble_x, y = bubble_y, width = bubble_width, height = 28, radius = 10,
    color = {0.22, 0.22, 0.22, emphasis}})
  base.stack({x = bubble_x, y = bubble_y, width = bubble_width, height = 28, children = {
    base.text({text = string.format("%.0f", value), size = 13, font = "sans-serif",
               color = {1, 1, 1, emphasis}, anchor = "center"}),
  }})
end
