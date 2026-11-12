local base = require("vw.base")
local datetime = require("vw.datetime")
local hyprland = require("vw.services.hyprland")

local function query(method, fallback)
  if not hyprland.available() then return fallback end
  local ok, value = pcall(method)
  return ok and value or fallback
end

local workspaces = query(hyprland.workspaces, {})
table.sort(workspaces, function(left, right) return left.id < right.id end)

local active_workspace = query(hyprland.active_workspace, { id = -1 })
local active_window = query(hyprland.active_window, { title = "Desktop" })
local title = active_window.title or "Desktop"
if #title > 42 then title = title:sub(1, 39) .. "..." end

if #workspaces == 0 then
  workspaces = { { id = 0, name = hyprland.available() and "empty" or "Hyprland offline" } }
end

base.row({
  anchor = "bottom",
  layer = "top",
  exclusive = true,
  margin = { 0, 0, 0, 0 },
  padding = 8,
  gap = 12,
  radius = 14,
  children = {
    base.image({
      path = os.getenv("HOME") .. "/.face",
      width = 32,
      height = 32,
      radius = 80,
    }),
    base.repeater({
      items = workspaces,
      render = function(workspace)
        local active = workspace.id == active_workspace.id
        return base.stack({
          width = 42,
          height = 28,
          children = {
            base.rectangle({
              width = 42,
              height = 28,
              radius = 30,
              color = active and { 0.8, 0.12, 0.18, 0.9 } or { 0.12, 0.14, 0.18, 0.72 },
              on_click = function(event)
                if event.button == 272 then
                  hyprland.request("/dispatch hl.dsp.focus({ workspace = '" .. workspace.id .. "' })")
                end
              end,
              hover = {
                color = { 1.0, 0.24, 0.32, 1.0 },
                scale = 1.06,
                duration = 0.14,
              },
            }),
            base.text({
              text = workspace.name,
              font = "JetBrains Mono NF",
              size = 14,
              weight = active and "bold" or "regular",
              color = active and { 1, 1, 1, 1 } or { 0.85, 0.87, 0.9, 0.9 },
              anchor = "center",
            }),
          },
        })
      end,
    }),
    base.rectangle({
      width = 1,
      height = 22,
      color = { 1, 1, 1, 0.2 },
      anchor = "center",
    }),
    base.text({
      text = title,
      font = "JetBrains Mono NF",
      size = 14,
      weight = "medium",
      color = { 0.9, 0.92, 0.95, 0.9 },
      anchor = "center",
    }),
    base.rectangle({
      width = 1,
      height = 22,
      color = { 1, 1, 1, 0.2 },
      anchor = "center",
    }),
    base.text({
      text = datetime.format("%a %H:%M:%S"),
      font = "JetBrains Mono NF",
      size = 14,
      weight = "bold",
      color = { 1, 1, 1, 1 },
      anchor = "center",
    }),
  },
})
