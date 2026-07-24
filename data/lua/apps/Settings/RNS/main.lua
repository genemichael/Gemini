-- Settings > RNS — network interface configuration for the RNS service.
-- AutoInterface (peer-to-peer on local WiFi) is always on and needs no
-- config; this screen manages the OPTIONAL TCP client used to reach a
-- transport node beyond the LAN. Off by default by design: the device
-- is a fully standalone RNS node and never depends on a remote daemon.
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

content:Label { text = "RNS Interfaces", w = lvgl.PCT(70), h = 26 }
local back_btn = content:Button { w = 50, h = 22 }
back_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
back_btn:onClicked(function() apps.go_home() end)

local status = content:Label { text = "", w = lvgl.PCT(100), h = 16 }

content:Label { text = "-- AutoInterface (WiFi LAN) --", w = lvgl.PCT(100), h = 16 }

-- Optional since 2026-07-23: on congested 2.4GHz networks its
-- multicast carrier flaps; TCP below is the reliable alternative.
-- Toggle persists via rns:setAuto(); takes effect at next reboot.
local auto = rns:getAuto()
local auto_enabled = auto.enabled
local auto_btn = content:Button { w = lvgl.PCT(44), h = 30 }
local auto_lbl = auto_btn:Label { text = "", align = lvgl.ALIGN.CENTER }
local function refresh_auto()
    auto_lbl:set { text = auto_enabled and "Enabled: yes" or "Enabled: no" }
end
refresh_auto()
auto_btn:onClicked(function()
    auto_enabled = not auto_enabled
    if rns:setAuto(auto_enabled) then
        refresh_auto()
        status:set { text = "AutoInterface " ..
            (auto_enabled and "on" or "off") .. " - reboot to apply" }
    else
        auto_enabled = not auto_enabled
        status:set { text = "Save failed (service offline?)" }
    end
end)
local auto_state = content:Label { w = lvgl.PCT(52), h = 30,
    text = auto.running and "running" or "not running" }

content:Label { text = "-- TCP transport (optional) --", w = lvgl.PCT(100), h = 16 }

local tcp = rns:getTcp()

-- Toggle button (no Checkbox precedent in this codebase; buttons are
-- the proven control everywhere else).
local tcp_enabled = tcp.enabled
local en_btn = content:Button { w = lvgl.PCT(44), h = 30 }
local en_lbl = en_btn:Label { text = "", align = lvgl.ALIGN.CENTER }
local function refresh_en()
    en_lbl:set { text = tcp_enabled and "Enabled: yes" or "Enabled: no" }
end
refresh_en()
en_btn:onClicked(function()
    tcp_enabled = not tcp_enabled
    refresh_en()
end)

content:Label { text = "Host", w = lvgl.PCT(20), h = 30 }
local host_ta = content:Textarea {
    one_line = true, text = tcp.host, w = lvgl.PCT(76), h = 30,
}
content:Label { text = "Port", w = lvgl.PCT(20), h = 30 }
local port_ta = content:Textarea {
    one_line = true, text = tostring(tcp.port), w = lvgl.PCT(40), h = 30,
}

local save_btn = content:Button { w = lvgl.PCT(32), h = 30 }
save_btn:Label { text = "Save", align = lvgl.ALIGN.CENTER }
save_btn:onClicked(function()
    local enabled = tcp_enabled
    local host = host_ta.text or ""
    local port = tonumber(port_ta.text) or 0
    if enabled and (host == "" or port < 1 or port > 65535) then
        status:set { text = "Need a host and a valid port to enable" }
        return
    end
    if rns:setTcp(enabled, host, port) then
        if enabled then
            status:set { text = "Saved - connecting to " .. host .. ":" .. port }
        else
            status:set { text = "Saved - TCP off (drops at next reboot)" }
        end
    else
        status:set { text = "Save failed (service offline?)" }
    end
end)

-- Live connection indicator, polled while the screen is open.
local conn = content:Label { text = "", w = lvgl.PCT(100), h = 16 }
local function refresh_conn()
    local t = rns:getTcp()
    conn:set { text = t.enabled
        and ("TCP: " .. (t.online and "connected" or "not connected"))
        or "TCP: disabled" }
end
refresh_conn()
apps.add_timer({ period = 2000, cb = refresh_conn })
