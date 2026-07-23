-- Settings > Names — the device's two identities in one place:
--   MeshCore node name  (LoRa mesh; what other MeshCore users see)
--   RNS announce name   (LXMF/WiFi; what Sideband/MeshChat peers see)
-- The two are independent on purpose: the mesh persona and the RNS
-- persona can differ. Native setters persist each to its own store.
local lvgl  = require("lvgl")
local apps  = require("lib/apps")
local nav   = require("lib/nav")
local theme = require("lib/theme")
local rns   = require("lib/rns")

local root = apps.new_root()
root:set { w = lvgl.HOR_RES(), h = lvgl.VER_RES(), pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

theme.show_background()

local content = root:Object {
    flex = { flex_direction = "row", flex_wrap = "wrap" },
    w = lvgl.HOR_RES(), h = lvgl.VER_RES(),
    border_width = 0, pad_all = 6, bg_opa = 0,
}
nav.replace(content, { flags = nav.ROLLOVER + nav.SCROLL_FIRST })

content:Label { text = "Names", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
back_btn:onClicked(function() apps.go_home() end)

local status = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

-- MeshCore node name -------------------------------------------------
content:Label { text = "-- MeshCore node name (LoRa) --", w = lvgl.PCT(100), h = 16 }

local mesh_name = ""
local ok, info = pcall(_mesh_get_node_info)
if ok and info and info.name then mesh_name = info.name end

local mesh_ta = content:Textarea {
    one_line = true, text = mesh_name, w = lvgl.PCT(68), h = 30,
}
local mesh_btn = content:Button { w = lvgl.PCT(28), h = 30 }
mesh_btn:Label { text = "Save", align = lvgl.ALIGN.CENTER }
mesh_btn:onClicked(function()
    local v = mesh_ta.text
    if v == nil or v == "" then
        status:set { text = "Mesh name cannot be empty" }
        return
    end
    local sok = pcall(_mesh_set_config, "name", v)
    status:set { text = sok and ("Mesh name saved: " .. v)
                        or "Mesh name save failed" }
end)

-- RNS announce name --------------------------------------------------
content:Label { text = "-- RNS announce name (WiFi) --", w = lvgl.PCT(100), h = 16 }

local rns_ta = content:Textarea {
    one_line = true, text = rns:getName(), w = lvgl.PCT(68), h = 30,
}
local rns_btn = content:Button { w = lvgl.PCT(28), h = 30 }
rns_btn:Label { text = "Save", align = lvgl.ALIGN.CENTER }
rns_btn:onClicked(function()
    local v = rns_ta.text or ""
    if rns:setName(v) then
        -- Setter re-announces, so peers see the new name immediately.
        status:set { text = (v == "") and "RNS name cleared (hash only)"
                            or ("RNS name saved + announced: " .. v) }
    else
        status:set { text = "RNS name save failed (service offline?)" }
    end
end)

content:Label {
    text = "Mesh name is instant on the mesh.\nRNS name re-announces to WiFi peers on save.",
    w = lvgl.PCT(100), h = 34,
}
