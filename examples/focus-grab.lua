local base = require("vw.base")
local surface = require("vw.surface")
local runtime = require("vw.toplevel")

base.rectangle({width = 360, height = 150, radius = 24, color = {0.07, 0.11, 0.15, 1}})
base.text({text = "Dismissible focus grab", x = 20, y = 16, size = 20,
            color = {0.9, 0.96, 1, 1}})
base.text_field({id = "focus-demo-query", x = 20, y = 58, width = 320, height = 40,
                  placeholder = "Type here", autofocus = true})
base.text({text = "Click outside to dismiss", x = 20, y = 116, size = 12,
            color = {0.55, 0.68, 0.75, 1}})
surface.set({width = 360, height = 150, anchor = "top", layer = "overlay",
  namespace = "vw:focus-grab-demo", margin = {24, 0, 0, 0}, focus_grab = true,
  on_activated = function() print("vw focus grab activated") end,
  on_dismiss = function(event)
    print("vw focus grab dismissed:", event.reason)
    if event.reason ~= "released" then runtime.close() end
  end})
