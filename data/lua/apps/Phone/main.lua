-- ══════════════════════════════════════════════════════════════════
-- Phone — LXST voice calls over Reticulum (Pyxis control plane). One
-- screen flow with three views: Picker (contacts + recent calls, the
-- default), In-Call (state/duration/mute/hangup), Incoming (answer/reject).
-- Mirrors data/lua/apps/RNS/main.lua's structure (header, clear_view,
-- gridnav_body list) and data/lua/apps/Contacts/main.lua's list styling.
-- Audio never crosses the Lua VM (CLAUDE.md hard rule) — this file is
-- control-plane only: dial/answer/hangup/state via lib/phone.lua.
-- ══════════════════════════════════════════════════════════════════

local lvgl = require("lvgl")
local phone = require("lib/phone")
local rns = require("lib/rns")
local contacts = require("lib/contacts")
local apps = require("lib/apps")
local nav = require("lib/nav")
local theme = require("lib/theme")
local utils = require("lib/utils")
local gridnav_body = require("lib/gridnav_body")

-- Optional: a simple ring tone on incoming calls. sound.generateTone/play is
-- a real, registered native binding (src/sound.cpp:_sound_generate_tone/
-- _sound_play — verified, not the C-side notify.cpp melody path, which has
-- no Lua entry point). Guarded: a missing/failing sound module must never
-- block the incoming-call view from showing.
local sound_ok, sound = pcall(require, "lib/sound")
sound_ok = sound_ok and sound ~= nil

local W = lvgl.HOR_RES()
local H = lvgl.VER_RES()
local HEADER_H = 24

local root = apps.new_root()
root:set { w = W, h = H, pad_all = 0, border_width = 0, bg_opa = 0 }
root:clear_flag(lvgl.FLAG.SCROLLABLE)

theme.show_background()

-- ── Palette ──────────────────────────────────────────────────────────────
local COL_META    = "#9aa0a6"
local COL_ONLINE  = "#7fe57f"
local COL_OFFLINE = "#ff8080"
local COL_DIM     = "#666666"
local COL_ACCENT  = "#7fb3ff"
local COL_ANSWER  = "#2e7d32"
local COL_REJECT  = "#8b2020"
local COL_STATE   = "#f0f0f0"

-- ── State ────────────────────────────────────────────────────────────────
local current_view = nil        -- body/overlay object of the active screen
local screen_mode = nil         -- "picker" | "incall" | "incoming"
local dur_timer = nil           -- ticking timer, only while ACTIVE
local incoming_ring_snd = nil   -- looping tone object while INCOMING view is up

local show_picker, show_incall, show_incoming

-- ── Header (persistent across screens) ──────────────────────────────────
local header = root:Object {
    w = W, h = HEADER_H, y = 0,
    border_width = 0, pad_left = 4, pad_right = 4, bg_opa = 0,
}
header:clear_flag(lvgl.FLAG.SCROLLABLE)
local header_title  = header:Label { text = "Phone", align = lvgl.ALIGN.LEFT_MID }
local header_status = header:Label { text = "", align = lvgl.ALIGN.RIGHT_MID, text_color = COL_META }

local function set_header(title)
    header_title.text = title or "Phone"
end

local function refresh_status()
    local ok, avail = pcall(function() return rns:available() end)
    avail = ok and avail or false
    header_status.text = avail and "Online" or "Offline"
    header_status:set { text_color = avail and COL_ONLINE or COL_OFFLINE }
    return avail
end

-- ── View teardown (mirrors RNS/Contacts' clear_view) ────────────────────
local function clear_view()
    nav.reset()
    if current_view then apps.delete_view(current_view); current_view = nil end
end

local function stop_ring()
    if incoming_ring_snd then
        pcall(function() incoming_ring_snd:stop() end)
        pcall(function() incoming_ring_snd:delete() end)
        incoming_ring_snd = nil
    end
end

local function stop_dur_timer()
    if dur_timer then pcall(function() dur_timer:delete() end); dur_timer = nil end
end

-- Resolve a name for a peer hash via the Address Book; falls back to a
-- shortened hash. Used by both the Recent list and the in-call/incoming
-- views.
local function name_for(peer_hex)
    if not peer_hex or peer_hex == "" then return "Unknown" end
    local ok, entry = pcall(function() return contacts:find_by_lxmf(peer_hex) end)
    if ok and entry and entry.name and entry.name ~= "" then return entry.name end
    return peer_hex:sub(1, 16)
end

-- ── Re-sync helper: whatever the native call state is right now, show the
-- matching view. Called at boot and whenever the app is reopened (its Lua
-- state is fresh, but a native call may already be in progress — HYBRID_PLAN
-- M3+: calls survive the app being closed since audio/FSM live natively).
local function sync_from_state()
    local ok, state = pcall(function() return phone:state() end)
    state = ok and state or "IDLE"
    if state == "IDLE" then
        show_picker()
    elseif state == "INCOMING_RINGING" then
        local okp, peer = pcall(function() return phone:peer() end)
        show_incoming(okp and peer or "")
    else
        show_incall(state)
    end
end

-- ══════════════════════════════════════════════════════════════════════
-- PICKER VIEW (default)
-- ══════════════════════════════════════════════════════════════════════
show_picker = function()
    screen_mode = "picker"
    stop_ring()
    stop_dur_timer()
    clear_view()
    set_header("Phone")
    refresh_status()

    local body = gridnav_body(root, HEADER_H, H - HEADER_H, nav.ROLLOVER + nav.SCROLL_FIRST, true)
    current_view = body

    local home_btn = body:Button { w = 60, h = 24 }
    home_btn:Label { text = "Home", align = lvgl.ALIGN.CENTER }
    home_btn:onevent(lvgl.EVENT.RELEASED, function() apps.go_home() end)

    local list = body:Object {
        w = lvgl.PCT(100), h = H - HEADER_H - 30,
        border_width = 0, pad_all = 0, bg_opa = 0,
        flex = { flex_direction = "column", flex_wrap = "nowrap" },
    }
    nav.list(list)
    local bind_click = nav.scroll_aware(list)

    -- Dial helper shared by both sections below.
    local function dial(dest_hex, label)
        local okd, avail = pcall(function() return rns:available() end)
        if not (okd and avail) then
            utils.createNotification(root, "RNS offline", 1800)
            return
        end
        local ok = phone:dial(dest_hex)
        if not ok then
            utils.createNotification(root, "Call failed", 1800)
            return
        end
        show_incall("CONNECTING")
    end

    -- Contacts section -----------------------------------------------------
    list:Label { text = "Contacts", w = lvgl.PCT(100), h = 16, text_color = COL_META }

    local ok_avail, rns_avail = pcall(function() return rns:available() end)
    rns_avail = ok_avail and rns_avail or false

    local entries = contacts:list()
    -- Alphabetical, same as Contacts app's sorted_entries().
    local sorted = {}
    for i, e in ipairs(entries) do sorted[i] = e end
    table.sort(sorted, function(a, b) return (a.name or ""):lower() < (b.name or ""):lower() end)

    if #sorted == 0 then
        list:Label {
            text = "No contacts yet. Add some in Contacts.",
            w = lvgl.PCT(100), h = 30, text_color = COL_META,
        }
    end
    for _, e in ipairs(sorted) do
        local callable = (e.rns_lxmf ~= nil and e.rns_lxmf ~= "") and rns_avail
        local row = list:Button { w = lvgl.PCT(100), h = 26 }
        local lbl = row:Label { text = e.name, align = lvgl.ALIGN.LEFT_MID }
        lbl:set { text_color = callable and COL_STATE or COL_DIM }
        local hint = row:Label {
            text = callable and "Call" or "--",
            align = lvgl.ALIGN.RIGHT_MID, text_color = callable and COL_ACCENT or COL_DIM,
        }
        if callable then
            bind_click(row, function() dial(e.rns_lxmf, e.name) end)
        end
    end

    -- Recent section ---------------------------------------------------------
    list:Label { text = "Recent", w = lvgl.PCT(100), h = 16, text_color = COL_META }

    local log = phone:log()
    if #log == 0 then
        list:Label {
            text = "No recent calls.", w = lvgl.PCT(100), h = 24, text_color = COL_META,
        }
    else
        -- Most recent first.
        for i = #log, 1, -1 do
            local rec = log[i]
            local name = name_for(rec.peer)
            local dir_tag = (rec.dir == "in" and "In") or (rec.dir == "out" and "Out") or "Missed"
            local when = (rec.ts and rec.ts > 0) and utils.relTime(rec.ts) or ""
            local row = list:Button { w = lvgl.PCT(100), h = 26 }
            local left = row:Label { text = dir_tag .. "  " .. name, align = lvgl.ALIGN.LEFT_MID }
            if rec.dir == "missed" then left:set { text_color = COL_OFFLINE } end
            row:Label { text = when, align = lvgl.ALIGN.RIGHT_MID, text_color = COL_META }
            local can_call = rec.peer and rec.peer ~= "" and rns_avail
            if can_call then
                bind_click(row, function() dial(rec.peer, name) end)
            end
        end
    end
end

-- ══════════════════════════════════════════════════════════════════════
-- IN-CALL VIEW (state ~= IDLE, not INCOMING_RINGING)
-- ══════════════════════════════════════════════════════════════════════
show_incall = function(initial_state)
    screen_mode = "incall"
    stop_ring()
    clear_view()

    local ok_peer, peer_hex = pcall(function() return phone:peer() end)
    peer_hex = ok_peer and peer_hex or ""
    local name = name_for(peer_hex)
    set_header(name)

    local body = gridnav_body(root, HEADER_H, H - HEADER_H, nav.ROLLOVER, true)
    current_view = body

    body:Label { text = name, w = lvgl.PCT(100), h = 20 }
    body:Label {
        text = (peer_hex ~= "" and peer_hex:sub(1, 16)) or "",
        w = lvgl.PCT(100), h = 16, text_color = COL_META,
    }

    local state_lbl = body:Label {
        text = initial_state or "CONNECTING", w = lvgl.PCT(100), h = 30, text_color = COL_STATE,
    }
    local dur_lbl = body:Label { text = "", w = lvgl.PCT(100), h = 20, text_color = COL_META }

    local function fmt_dur(sec)
        sec = sec or 0
        return string.format("%02d:%02d", math.floor(sec / 60), sec % 60)
    end

    stop_dur_timer()
    dur_timer = apps.add_timer {
        period = 1000,
        cb = function()
            local oks, s = pcall(function() return phone:state() end)
            s = oks and s or "IDLE"
            if s == "IDLE" then
                -- Call ended natively while this timer was running (e.g. peer
                -- hung up) — fall back to the picker.
                show_picker()
                return
            end
            state_lbl.text = s
            if s == "ACTIVE" then
                local okd, d = pcall(function() return phone:duration() end)
                dur_lbl.text = fmt_dur(okd and d or 0)
            else
                dur_lbl.text = ""
            end
        end,
    }

    local ctrl_row = body:Object {
        w = lvgl.PCT(100), h = 36, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    }
    ctrl_row:clear_flag(lvgl.FLAG.SCROLLABLE)

    local ok_m, muted = pcall(function() return phone:muted() end)
    muted = ok_m and muted or false
    local mute_btn = ctrl_row:Button { w = lvgl.PCT(48), h = 32 }
    local mute_lbl = mute_btn:Label { text = muted and "Unmute" or "Mute", align = lvgl.ALIGN.CENTER }
    mute_btn:onevent(lvgl.EVENT.RELEASED, function()
        local ok_cur, cur = pcall(function() return phone:muted() end)
        cur = ok_cur and cur or false
        local ok_set = phone:setMute(not cur)
        if ok_set then mute_lbl.text = (not cur) and "Unmute" or "Mute" end
    end)

    local hangup_btn = ctrl_row:Button { w = lvgl.PCT(48), h = 32, bg_color = COL_REJECT }
    hangup_btn:Label { text = "Hangup", align = lvgl.ALIGN.CENTER }
    hangup_btn:onevent(lvgl.EVENT.RELEASED, function()
        phone:hangup()
        show_picker()
    end)

    -- Live state updates while this view is showing (in addition to the
    -- 1s-tick timer above, so state text updates immediately on transition,
    -- not up to a second late).
    phone:onStateChange(function(state_name, peer, ts)
        if screen_mode ~= "incall" then return end
        if state_name == "IDLE" then
            show_picker()
        else
            state_lbl.text = state_name
        end
    end)
end

-- ══════════════════════════════════════════════════════════════════════
-- INCOMING VIEW (via onIncoming)
-- ══════════════════════════════════════════════════════════════════════
show_incoming = function(peer_hex)
    screen_mode = "incoming"
    stop_dur_timer()
    clear_view()

    local name = name_for(peer_hex)
    set_header("Incoming call")

    local body = gridnav_body(root, HEADER_H, H - HEADER_H, nav.ROLLOVER, true)
    current_view = body

    body:Label { text = name, w = lvgl.PCT(100), h = 24 }
    body:Label {
        text = (peer_hex ~= "" and peer_hex:sub(1, 16)) or "",
        w = lvgl.PCT(100), h = 16, text_color = COL_META,
    }
    body:Label { text = "Incoming call...", w = lvgl.PCT(100), h = 24, text_color = COL_STATE }

    -- Best-effort ring tone. Failure here (module missing, native call
    -- errors) must never block Answer/Reject from working.
    if sound_ok then
        local ok_snd, snd = pcall(function()
            return sound.generateTone(880, 500, { waveform = "sine" })
        end)
        if ok_snd and snd then
            incoming_ring_snd = snd
            pcall(function() snd:setLoop(true) end)
            pcall(function() snd:play() end)
        end
    end

    local ctrl_row = body:Object {
        w = lvgl.PCT(100), h = 40, border_width = 0, pad_all = 0,
        flex = { flex_direction = "row", flex_wrap = "nowrap" },
    }
    ctrl_row:clear_flag(lvgl.FLAG.SCROLLABLE)

    local answer_btn = ctrl_row:Button { w = lvgl.PCT(48), h = 36, bg_color = COL_ANSWER }
    answer_btn:Label { text = "Answer", align = lvgl.ALIGN.CENTER }
    answer_btn:onevent(lvgl.EVENT.RELEASED, function()
        stop_ring()
        if phone:answer() then
            show_incall("CONNECTING")
        else
            show_picker()
        end
    end)

    local reject_btn = ctrl_row:Button { w = lvgl.PCT(48), h = 36, bg_color = COL_REJECT }
    reject_btn:Label { text = "Reject", align = lvgl.ALIGN.CENTER }
    reject_btn:onevent(lvgl.EVENT.RELEASED, function()
        stop_ring()
        phone:hangup()
        show_picker()
    end)

    -- If the caller hangs up before we answer, the state goes back to IDLE —
    -- follow it back to the picker.
    phone:onStateChange(function(state_name, peer, ts)
        if screen_mode ~= "incoming" then return end
        if state_name == "IDLE" then
            stop_ring()
            show_picker()
        end
    end)
end

-- ── Boot ─────────────────────────────────────────────────────────────────
-- Clear every callback slot on exit, mirroring RNS/main.lua — a stale
-- closure over a deleted screen must never fire again. The call itself
-- (native FSM + audio) is untouched by closing this app; reopening re-syncs
-- via sync_from_state() below.
apps.set_on_close(function()
    stop_ring()
    stop_dur_timer()
    phone:onIncoming(nil)
    phone:onStateChange(nil)
    phone:onMissed(nil)
end)

phone:onIncoming(function(peer_hex, ts)
    -- An incoming call always takes over the screen immediately, regardless
    -- of what view is currently showing (picker or an unrelated in-call
    -- view — the native engine only allows one call at a time, so this can
    -- only fire while IDLE).
    show_incoming(peer_hex)
end)

phone:onMissed(function(peer_hex, ts)
    if screen_mode == "incoming" then
        stop_ring()
        show_picker()
    elseif screen_mode == "picker" then
        show_picker()   -- refresh Recent list
    end
end)

-- Poll availability like RNS/main.lua so the header status line stays live
-- without needing its own dedicated event.
apps.add_timer { period = 3000, cb = function()
    if screen_mode == "picker" then refresh_status() end
end }

sync_from_state()

return root
