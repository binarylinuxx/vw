local base = require("vw.base")
local session = require("vw.session")

session.lock({ pam_conf = os.getenv("VW_LOCK_PAM_CONF") })
local display = session.surface()
session.locksurface({})

local width, height = display.width, display.height
base.rectangle({ width = width, height = height, color = { 0.025, 0.035, 0.055, 1 } })

local panel_width = math.min(360, width - 32)
local x = math.max(0, (width - panel_width) / 2)
local y = math.max(0, (height - 170) / 2)
base.rectangle({ x = x, y = y, width = panel_width, height = 170,
                 radius = 16, color = { 0.09, 0.11, 0.15, 1 } })
base.text({ text = "Session locked", x = x + 20, y = y + 18,
            size = 23, color = { 0.95, 0.96, 1, 1 } })
base.text({ text = display.output, x = x + 20, y = y + 54,
            size = 12, color = { 0.55, 0.63, 0.75, 1 } })
base.text_field({ id = "lock-password", x = x + 20, y = y + 82,
                  width = panel_width - 40, height = 44,
                  placeholder = "Password", password = true, authenticate = true })
if session.failed() then
  base.text({ text = "Authentication failed", x = x + 20, y = y + 140,
              size = 12, color = { 1, 0.4, 0.43, 1 } })
end
