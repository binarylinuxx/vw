local base = require("vw.base")
local window = require("vw.toplevel")
local directory = debug.getinfo(1, "S").source:match("^@(.*/)") or "examples/"

window.set({title = "vw — JavaScript Canvas", app_id = "vw-canvas-demo", width = 660, height = 440})
local size = window.size()
base.canvas({id = "orbit-canvas", width = size.width, height = size.height,
             script = directory .. "canvas.js", animate = true})
