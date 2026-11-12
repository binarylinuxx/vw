local base = require("vw.base")
local toplevel = require("vw.toplevel")

toplevel.set({ title = "vw Toplevel Example", app_id = "vw-toplevel-example",
               width = 640, height = 400 })

local size = toplevel.size()
base.rectangle({ width = size.width, height = size.height,
                 color = { 0.075, 0.09, 0.13, 1 } })
base.text({ text = "A regular Wayland window", font = "sans-serif", size = 24,
            x = 28, y = 28, anchor = "top_left",
            color = { 0.92, 0.95, 1, 1 } })
base.text({ text = "Resize the window; this scene follows the configured size.",
            font = "sans-serif", size = 14, x = 28, y = 76, anchor = "top_left",
            color = { 0.58, 0.67, 0.8, 1 } })
base.text_field({ id = "toplevel-search", width = math.max(140, size.width - 56),
                  height = 42, x = 28, y = 128, anchor = "top_left",
                  placeholder = "Type here..." })
base.rectangle({ width = 100, height = 38, x = 28, y = size.height - 62,
                 anchor = "top_left", radius = 10,
                 color = { 0.65, 0.15, 0.22, 1 },
                 on_click = function() toplevel.close() end })
base.text({ text = "Close", x = 58, y = size.height - 54, anchor = "top_left",
            color = { 1, 1, 1, 1 } })
