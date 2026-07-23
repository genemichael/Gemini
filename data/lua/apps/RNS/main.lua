-- ══════════════════════════════════════════════════════════════════
-- RNS — minimal LXMF messenger over Reticulum (Pyxis control plane).
-- Two screens: Announces (contacts seen via rns.announces()) and Thread
-- (one contact's messages + compose). No settings, no persistence beyond
-- what lib/rns.lua already keeps in memory. See lib/rns.lua for the
-- native-bridge contract and data/lua/apps/Messenger/main.lua for the
-- list/thread/gridnav_body pattern this mirrors (studied, not copied —
-- MeshCore's contact/channel machinery does not apply here).
-- ══════════════════════════════════════════════════════════════════

local lvgl = require("lvgl")
local rns = require("lib/rns")
local apps = require("lib/apps")
local nav = require("lib/nav")
local theme = require("lib/theme")
local utils = require("lib/utils")
local gridnav_body = require("lib/gridnav_body")
local contacts = require("lib/contacts")

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()
local HEADER_H = 24

local root = apps.new_root()
root:set { w = W, h = H, pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

-- Lightweight app: wallpaper behind everything (header/bodies stay transparent).
theme.show_background()

-- ── Palette (fixed, like Messenger's bubble colors — not theme-driven;
-- default widgets around them still inherit the active theme). ──────────────
local COL_ME_BG    = "#0b3d2e"
local COL_ME_TX    = "#d7f5e6"
local COL_THEM_BG  = "#262626"
local COL_THEM_TX  = "#f0f0f0"
local COL_META     = "#9aa0a6"
local COL_ACCENT   = "#7fb3ff"
local COL_ONLINE   = "#7fe57f"
local COL_OFFLINE  = "#ff8080"

-- ── State ────────────────────────────────────────────────────────────────
local current_view = nil    -- body object of the active screen (torn down on swap)
local screen_mode = nil     -- "contacts" | "thread" | "offline"
local unread = {}           -- dest_hex -> true, cleared when that thread opens

-- ── Header (persistent across both screens) ─────────────────────────────
local header = root:Object {
    w = W, h = HEADER_H, y = 0,
    border_width = 0, pad_left = 4, pad_right = 4, bg_opa = 0,
}
header:clear_flag(lvgl.FLAG.SCROLLABLE)
local header_title  = header:Label { text = "RNS", align = lvgl.ALIGN.LEFT_MID }
local header_status = header:Label { text = "", align = lvgl.ALIGN.RIGHT_MID, text_color = COL_META }

local function set_header(title)
    header_title.text = title or "RNS"
end

-- Pulls both onStatus and available() per the spec: pushed status changes
-- refresh immediately, the poll timer below covers everything else cheaply.
local function refresh_status()
    local ok, avail = pcall(function() return rns:available() end)
    avail = ok and avail or false
    local okd, dest = pcall(function() return rns:dest() end)
    local dest_short = (okd and dest) and dest:sub(1, 16) or "no id"
    header_status.text = (avail and "Online  " or "Offline  ") .. dest_short
    header_status:set { text_color = avail and COL_ONLINE or COL_OFFLINE }
    return avail
end

-- Resolve a peer's display name: the user's own Address Book entry wins,
-- then the announce's broadcast name, then a truncated hex fallback.
-- Looked up fresh every call (never cached) so a Contacts rename shows up
-- next time a row/thread renders. pcall-wrapped: a contacts store problem
-- must never break this app.
local function resolve_name(dest_hex, announce_name)
    local ok, entry = pcall(function() return contacts:find_by_lxmf(dest_hex) end)
    if ok and entry and entry.name and entry.name ~= "" then
        return entry.name
    end
    if announce_name and announce_name ~= "" then
        return announce_name
    end
    return dest_hex:sub(1, 16)
end

-- ── View teardown (mirrors Messenger's clear_view: drop the nav stack, then
-- chunk-delete the outgoing body so a big list doesn't trip the watchdog). ──
local function clear_view()
    nav.reset()
    if current_view then apps.delete_view(current_view); current_view = nil end
end

local show_contacts, show_thread, show_offline

-- ── OFFLINE SCREEN ──────────────────────────────────────────────────────
show_offline = function()
    screen_mode = "offline"
    clear_view()
    set_header("RNS")

    local body = gridnav_body(root, HEADER_H, H - HEADER_H, nav.ROLLOVER, true)
    current_view = body

    body:Label { text = "RNS service offline", w = lvgl.PCT(100) }
    body:Label {
        text = "Waiting for the Pyxis service to start...",
        w = lvgl.PCT(100), text_color = COL_META,
    }
    local home_btn = body:Button { w = 80, h = 28 }
    home_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
    home_btn:onevent(lvgl.EVENT.RELEASED, function() apps.go_home() end)
end

-- ── ANNOUNCES / CONTACTS SCREEN (default) ───────────────────────────────
show_contacts = function()
    screen_mode = "contacts"
    clear_view()
    set_header("Announces")

    local body = gridnav_body(root, HEADER_H, H - HEADER_H, nav.ROLLOVER + nav.SCROLL_FIRST, true)
    current_view = body

    local home_btn = body:Button { w = 60, h = 24 }
    home_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
    home_btn:onevent(lvgl.EVENT.RELEASED, function() apps.go_home() end)

    local ann_btn = body:Button { w = 90, h = 24 }
    ann_btn:Label { text = "Announce", align = lvgl.ALIGN.CENTER }
    ann_btn:onevent(lvgl.EVENT.RELEASED, function()
        local ok = rns:announce()
        utils.createNotification(root, ok and "Announced" or "Announce failed", 1800)
    end)

    local LIST_H = H - HEADER_H - 36
    local list = body:Object {
        w = lvgl.PCT(100), h = LIST_H,
        border_width = 0, pad_all = 0, bg_opa = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    -- Tap/click enters row-select (trackball steps rows), 'q' returns to the
    -- buttons above; touch drags scroll instead of opening a row.
    nav.list(list)
    local bind_click = nav.scroll_aware(list)

    -- dest_hex -> { row = <Button>, entry = <announce entry table> }
    local contact_rows = {}

    local function fill_row(row, entry)
        row:clean()
        local label = resolve_name(entry.dest, entry.name)
        local dot = unread[entry.dest] and "* " or ""
        local left = row:Label { text = dot .. label, align = lvgl.ALIGN.LEFT_MID }
        if unread[entry.dest] then left:set { text_color = COL_ACCENT } end
        local seen = (entry.last_seen and entry.last_seen > 0) and utils.relTime(entry.last_seen) or ""
        row:Label { text = seen, align = lvgl.ALIGN.RIGHT_MID, text_color = COL_META }
    end

    -- Create-or-update a row for one announce entry. rns.lua updates entries
    -- IN PLACE and never reorders __announces, so an existing row just gets
    -- refilled — no index bookkeeping needed.
    local function touch_row(entry)
        local e = contact_rows[entry.dest]
        if e then
            e.entry = entry
            fill_row(e.row, entry)
            return
        end
        local row = list:Button { w = lvgl.PCT(100), h = 26 }
        fill_row(row, entry)
        contact_rows[entry.dest] = { row = row, entry = entry }
        bind_click(row, function()
            local cur = contact_rows[entry.dest]
            if not cur then return end
            local nm = (cur.entry.name and cur.entry.name ~= "") and cur.entry.name or nil
            show_thread(cur.entry.dest, nm)
        end)
    end

    local announces = rns:announces()
    for _, entry in ipairs(announces) do touch_row(entry) end
    if #announces == 0 then
        list:Label {
            text = "No announces yet. Waiting for peers...",
            w = lvgl.PCT(100), h = 40, text_color = COL_META,
        }
    end

    -- Live updates while this screen is showing.
    rns:onAnnounce(touch_row)
    rns:onMessage(function(msg)
        unread[msg.from] = true
        local e = contact_rows[msg.from]
        if e then fill_row(e.row, e.entry) end
    end)
    rns:onDelivered(nil)   -- only meaningful inside an open thread
end

-- ── THREAD SCREEN ────────────────────────────────────────────────────────
show_thread = function(dest_hex, display_name)
    screen_mode = "thread"
    unread[dest_hex] = nil
    clear_view()
    display_name = resolve_name(dest_hex, display_name)
    set_header(display_name)

    local body = gridnav_body(root, HEADER_H, H - HEADER_H, nav.ROLLOVER + nav.SCROLL_FIRST, true)
    current_view = body

    local back = body:Button { w = 60, h = 24 }
    back:Label { text = "< Back", align = lvgl.ALIGN.CENTER }
    back:onevent(lvgl.EVENT.RELEASED, function() show_contacts() end)

    -- Message pane: a plain scrollable, focusable direct child of the gridnav
    -- body (Read Me's pattern) — SCROLL_FIRST scrolls it via trackball before
    -- moving focus on; no per-message selection here, so it needs none of
    -- Messenger's row-select scaffolding. Keep default SCROLLABLE+CLICKABLE.
    local MSG_H = H - HEADER_H - 24 - 36 - 8
    local msg_list = body:Object {
        w = lvgl.PCT(100), h = MSG_H,
        border_width = 0, pad_all = 2, bg_opa = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    -- Reading-content font role, if the active theme set one (no-op otherwise).
    local okf, text_font = pcall(lvgl.Font, "text", 16)
    if okf and text_font then msg_list:set { text_font = text_font } end

    local hash_labels = {}   -- msg.hash -> { label = <Label>, msg = <msg> }, for onDelivered

    local function render_msg(msg)
        local is_out = msg.dir == "out"
        local bubble = msg_list:Object {
            w = lvgl.PCT(88), h = lvgl.SIZE_CONTENT,
            bg_color = is_out and COL_ME_BG or COL_THEM_BG,
            bg_opa = 255, radius = 6, border_width = 0,
            pad_all = 4, pad_bottom = 5,
            flex = { flex_direction = "column", flex_wrap = "nowrap" },
        }
        bubble:clear_flag(lvgl.FLAG.SCROLLABLE)
        bubble:clear_flag(lvgl.FLAG.CLICKABLE)

        local head = (is_out and "You" or (display_name or dest_hex:sub(1, 16)))
            .. "  " .. utils.clockHM(msg.timestamp)
        if is_out then head = head .. "  " .. (msg.delivered and "delivered" or "sent") end
        local head_lbl = bubble:Label {
            text = head, w = lvgl.PCT(100), text_color = COL_META,
        }
        if is_out and msg.hash then hash_labels[msg.hash] = { label = head_lbl, msg = msg } end

        bubble:Label {
            text = msg.text or "", w = lvgl.PCT(100),
            text_color = is_out and COL_ME_TX or COL_THEM_TX,
        }
        return bubble
    end

    local history = rns:messages(dest_hex)   -- oldest first, cap 50 (rns.lua)
    local last_bubble
    for _, msg in ipairs(history) do last_bubble = render_msg(msg) end
    if last_bubble then last_bubble:scroll_to_view(false) end
    if #history == 0 then
        msg_list:Label {
            text = "No messages yet -- say hello.",
            text_color = COL_META, w = lvgl.PCT(100),
        }
    end

    -- Live updates while this thread is open.
    rns:onMessage(function(msg)
        if msg.from == dest_hex then
            local lbl = render_msg(msg)
            if lbl then lbl:scroll_to_view(false) end
        else
            unread[msg.from] = true
        end
    end)
    rns:onDelivered(function(hash_hex, ts)
        local e = hash_labels[hash_hex]
        if not e then return end
        pcall(function()
            e.msg.delivered = true
            e.label.text = "You  " .. utils.clockHM(e.msg.timestamp) .. "  delivered"
        end)
    end)
    rns:onAnnounce(function(entry)
        if entry.dest == dest_hex then
            display_name = resolve_name(dest_hex, entry.name)
            set_header(display_name)
        end
    end)

    -- Compose row.
    local textArea = body:Textarea {
        one_line = true, max_length = 400,
        placeholder_text = "Message",
        w = lvgl.PCT(72), h = 36,
    }
    if okf and text_font then textArea:set { text_font = text_font } end

    local function do_send()
        local text = textArea.text
        if not text or #text == 0 then return end
        local hash, err = rns:send(dest_hex, text)
        if not hash then
            utils.createNotification(root, "Send failed: " .. tostring(err or "unknown"), 2500)
            return
        end
        local msg = rns:appendSent(dest_hex, text, utils.now(), hash)
        local lbl = render_msg(msg)
        if lbl then lbl:scroll_to_view(false) end
        textArea.text = ""
    end

    textArea:onevent(lvgl.EVENT.KEY, function()
        local indev = lvgl.indev.get_act()
        if indev:get_key() == lvgl.KEY.ENTER then do_send() end
    end)

    local send_btn = body:Button { w = lvgl.SIZE_CONTENT, h = 36 }
    send_btn:Label { text = "Send", align = lvgl.ALIGN.CENTER }
    send_btn:onevent(lvgl.EVENT.RELEASED, do_send)
end

-- ── Boot ─────────────────────────────────────────────────────────────────
-- Clear our four callback slots on exit regardless of exit path (Exit-style
-- button here just calls apps.go_home(), and so does the Alt+Backspace home
-- chord) — a stale closure over a deleted screen must never fire again.
apps.set_on_close(function()
    rns:onMessage(nil)
    rns:onDelivered(nil)
    rns:onAnnounce(nil)
    rns:onStatus(nil)
end)

-- Availability can flip while the app is open; only auto-navigate off the
-- default contacts screen (never yank the user out of an open thread).
local function check_availability()
    local avail = refresh_status()
    if avail and screen_mode == "offline" then
        show_contacts()
    elseif (not avail) and screen_mode == "contacts" then
        show_offline()
    end
end
rns:onStatus(check_availability)
apps.add_timer { period = 3000, cb = check_availability }

local ok0, avail0 = pcall(function() return rns:available() end)
refresh_status()
if ok0 and avail0 then show_contacts() else show_offline() end

return root
