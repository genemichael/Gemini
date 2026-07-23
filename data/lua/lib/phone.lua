-- Phone / LXST call control-plane module
-- Bridges the native Pyxis call engine (src/phone_bridge.cpp) <-> Lua UI.
--
-- Call control-plane only: dial/answer/hangup/state/duration/peer/mute and
-- the three native-pushed call-lifecycle events. No audio here or anywhere
-- in Lua — audio never crosses the Lua VM (see CLAUDE.md hard rule); the
-- native call engine (PyxisCall.{h,cpp} + lxst_audio) owns it entirely.
--
-- Shape mirrors lib/rns.lua: thin native accessors, pcall-guarded callback
-- registration, and a small in-memory window (the call log) fed by native
-- dispatch. Unlike rns.lua's message/announce windows, the call log is also
-- appended to from THIS module's own M:dial() (an optimistic "outgoing"
-- entry), same idea as rns.lua's M:appendSent().

local M = {
    __onIncoming    = nil,   -- cb(peer_hex, ts)
    __onStateChange = nil,   -- cb(state_name, peer_hex, ts)
    __onMissed      = nil,   -- cb(peer_hex, ts)

    __log = {},   -- array, oldest first, cap 20: {peer, dir="in"|"out"|"missed", ts, duration}
}

local LOG_CAP = 20

-- Same trim shape as rns.lua/messages.lua's trim_list: drop from the front
-- so the most recent entries survive.
local function trim_log()
    local n = #M.__log
    if n <= LOG_CAP then return end
    local drop = n - LOG_CAP
    for i = 1, LOG_CAP do M.__log[i] = M.__log[i + drop] end
    for i = LOG_CAP + 1, n do M.__log[i] = nil end
end

-- Tracks the active/last call's log entry so state-change dispatches can
-- fill in `duration` when the call ends, without rescanning the log.
local __active_entry = nil

-- ── Native accessors ─────────────────────────────────────────────────────

-- dest_hex: any destination hash whose identity is cached (an LXMF delivery
-- hash from an announce works — the engine derives the peer's lxst.telephony
-- destination itself). Returns true on success (call initiation started),
-- false on bad hash or engine-level failure.
function M:dial(dest_hex)
    local ok, started = pcall(_phone_dial, tostring(dest_hex or ""))
    started = ok and started or false
    if started then
        local okt, t = pcall(_rtc_time)
        local ts = (okt and t and t > 0) and t or os.time()
        __active_entry = { peer = dest_hex, dir = "out", ts = ts, duration = 0 }
        table.insert(M.__log, __active_entry)
        trim_log()
    end
    return started
end

function M:answer()
    local ok, res = pcall(_phone_answer)
    return ok and res or false
end

function M:hangup()
    local ok, res = pcall(_phone_hangup)
    return ok and res or false
end

function M:state()
    local ok, s = pcall(_phone_state)
    return (ok and s) or "IDLE"
end

function M:duration()
    local ok, d = pcall(_phone_duration)
    return (ok and d) or 0
end

function M:peer()
    local ok, p = pcall(_phone_peer)
    return (ok and p) or ""
end

function M:setMute(muted)
    local ok, res = pcall(_phone_mute, muted and true or false)
    return ok and res or false
end

function M:muted()
    local ok, m = pcall(_phone_muted)
    return ok and m or false
end

-- ── Callback registration ────────────────────────────────────────────────

function M:onIncoming(cb)
    M.__onIncoming = cb
end

function M:onStateChange(cb)
    M.__onStateChange = cb
end

function M:onMissed(cb)
    M.__onMissed = cb
end

-- ── In-memory call log ───────────────────────────────────────────────────

-- Last 20 calls, oldest first: {peer, dir="in"|"out"|"missed", ts, duration}.
function M:log()
    return M.__log
end

-- ── Dispatch (called from C++ phone_bridge.cpp) ─────────────────────────
-- Plain module functions, no self — C++ calls them via lua_getfield +
-- lua_pcall with positional args only, same convention as lib/rns.lua's
-- __dispatch_msg/etc.

-- Signature: __dispatch_incoming(peer_hex, ts)
function M.__dispatch_incoming(peer_hex, ts)
    __active_entry = { peer = peer_hex, dir = "in", ts = ts, duration = 0 }
    table.insert(M.__log, __active_entry)
    trim_log()

    if M.__onIncoming then
        local ok, err = pcall(M.__onIncoming, peer_hex, ts)
        if not ok then print("[phone] onIncoming callback error: " .. tostring(err)) end
    end
end

-- Signature: __dispatch_call_state(state_name, peer_hex, ts)
function M.__dispatch_call_state(state_name, peer_hex, ts)
    if state_name == "IDLE" and __active_entry then
        -- Call just ended: freeze the final duration on the log entry.
        local ok, d = pcall(_phone_duration)
        __active_entry.duration = (ok and d) or __active_entry.duration or 0
        __active_entry = nil
    end

    if M.__onStateChange then
        local ok, err = pcall(M.__onStateChange, state_name, peer_hex, ts)
        if not ok then print("[phone] onStateChange callback error: " .. tostring(err)) end
    end
end

-- Signature: __dispatch_missed(peer_hex, ts)
function M.__dispatch_missed(peer_hex, ts)
    -- The ring that led to this miss already logged an "in" entry via
    -- __dispatch_incoming; recolor it "missed" rather than double-logging.
    if __active_entry and __active_entry.peer == peer_hex and __active_entry.dir == "in" then
        __active_entry.dir = "missed"
        __active_entry = nil
    else
        table.insert(M.__log, { peer = peer_hex, dir = "missed", ts = ts, duration = 0 })
        trim_log()
    end

    if M.__onMissed then
        local ok, err = pcall(M.__onMissed, peer_hex, ts)
        if not ok then print("[phone] onMissed callback error: " .. tostring(err)) end
    end
end

return M
