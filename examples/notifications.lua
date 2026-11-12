-- Native notification daemon with top-right layer-shell popup cards.
local base = require("vw.base")
local surface = require("vw.surface")
local notifications = require("vw.services.notifications")

local started, error = notifications.start()
assert(started, error)
local records = notifications.popups()
local width, card_height, gap = 360, 152, 10
local count = math.min(3, #records)
local C = {panel = {0.06, 0.09, 0.12, 0.98}, text = {0.94, 0.97, 1, 1},
           muted = {0.57, 0.67, 0.73, 1}, accent = {0.5, 0.85, 0.77, 1}}

if count == 0 then
  base.rectangle({width = 1, height = 1, visible = false})
  surface.set({width = 1, height = 1, anchor = "top_right", layer = "overlay",
               namespace = "vw:notifications", input_regions = {}})
else
  for index = 1, count do
    local item = records[index]
    local y = (index - 1) * (card_height + gap)
    base.rectangle({x = 0, y = y, width = width, height = card_height, radius = 18,
      color = C.panel, border_width = 1,
      border_color = item.urgency == 2 and {1, 0.48, 0.48, 0.6} or {0.28, 0.4, 0.45, 0.5}})
    local text_x = 16
    if item.app_icon ~= "" then
      local ok = pcall(base.image, {icon = item.app_icon, width = 24, height = 24, x = 16, y = y + 12})
      if ok then text_x = 48 end
    end
    base.text({text = item.app_name, x = text_x, y = y + 15, font = "sans-serif", size = 11,
               max_width = width - text_x - 48, max_lines = 1, color = C.muted})
    base.text({text = item.summary, x = 16, y = y + 43, font = "sans-serif", size = 16,
               max_width = width - 32, max_lines = 1, color = C.text})
    base.text({text = item.body, x = 16, y = y + 72, font = "sans-serif", size = 12,
               max_width = width - 32, max_lines = 2, color = C.muted})
    base.text({text = "×", x = width - 29, y = y + 10, font = "sans-serif", size = 20, color = C.muted})
    base.hit_area({x = width - 42, y = y + 4, width = 36, height = 32,
      on_click = function() notifications.dismiss(item.id) end})
    for action_index, action in ipairs(item.actions) do
      if action_index > 3 then break end
      local x = 16 + (action_index - 1) * 110
      base.rectangle({x = x, y = y + 118, width = 102, height = 26, radius = 9,
        color = {0.12, 0.2, 0.23, 1}, hover = {color = {0.18, 0.3, 0.32, 1}},
        on_click = function() notifications.invoke(item.id, action.key) end})
      base.text({text = action.label, x = x + 9, y = y + 123, font = "sans-serif", size = 11,
                 max_width = 84, max_lines = 1, color = C.accent})
    end
  end
  local regions = {}
  for index = 1, count do
    regions[index] = {0, (index - 1) * (card_height + gap), width, card_height}
  end
  surface.set({width = width, height = count * card_height + (count - 1) * gap,
    anchor = "top_right", layer = "overlay", margin = {14, 14, 0, 0},
    namespace = "vw:notifications", input_regions = regions})
end
