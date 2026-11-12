local base = require("vw.base")
local window = require("vw.toplevel")
local pipewire = require("vw.services.pipewire")

window.set({title = "vw — PipeWire Audio", app_id = "vw-pipewire-demo", width = 620, height = 400})
local size = window.size()
local output = pipewire.default_sink()
local input = pipewire.default_source()
local connected = pipewire.available()
local C = {background = {0.04, 0.065, 0.09, 1}, card = {0.08, 0.12, 0.16, 1},
           text = {0.92, 0.97, 1, 1}, muted = {0.52, 0.64, 0.7, 1}, accent = {0.5, 0.86, 0.77, 1}}
local function text(value, x, y, font_size, color)
  return base.text({text = tostring(value), x = x, y = y, font = "sans-serif", size = font_size or 14,
    color = color or C.text, max_width = math.max(1, size.width - x - 24), max_lines = 1})
end
local function button(value, x, y, width, callback)
  base.rectangle({x = x, y = y, width = width, height = 34, radius = 12, color = C.card,
                  hover = {color = {0.15, 0.23, 0.27, 1}}, on_click = callback})
  text(value, x + 12, y + 8, 12)
end
base.rectangle({width = size.width, height = size.height, color = C.background})
text("PipeWire audio", 24, 20, 25)
text(connected and "Native nodes · volume · mute · default devices" or pipewire.error(), 24, 58, 12, C.muted)
text("OUTPUT", 24, 103, 11, C.muted)
text(output and output.description or "No default output", 24, 126, 16)
text(output and string.format("%.0f%%  %s", output.volume * 100, output.muted and "· muted" or "") or "—",
     24, 154, 24, C.accent)
if output and output.can_control then
  button("− 5%", 24, 197, 84, function()
    local current = pipewire.default_sink()
    if current then pipewire.set_volume(current.id, math.max(0, current.volume - 0.05)) end
  end)
  button(output.muted and "Unmute" or "Mute", 119, 197, 90,
         function() pipewire.toggle_mute(output.id) end)
  button("+ 5%", 220, 197, 84, function()
    local current = pipewire.default_sink()
    if current then pipewire.set_volume(current.id, math.min(1, current.volume + 0.05)) end
  end)
end
text("INPUT", 24, 260, 11, C.muted)
text(input and input.description or "No default input", 24, 282, 16)
text(input and string.format("%.0f%%  %s", input.volume * 100, input.muted and "· muted" or "") or "—",
     24, 309, 18, C.accent)
text(#pipewire.sinks() .. " outputs · " .. #pipewire.sources() .. " inputs · " .. #pipewire.streams() .. " streams",
     24, 358, 11, C.muted)
if os.getenv("VW_PIPEWIRE_DEBUG") == "1" then
  print("PipeWire", connected, "nodes", #pipewire.nodes())
  if output then print("sink", output.id, output.name, "volume", output.volume,
                        "raw", output.linear_volume, "muted", output.muted, "control", output.can_control) end
  if input then print("source", input.id, input.name, "volume", input.volume, "muted", input.muted) end
end
