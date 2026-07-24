-- RNS/LXMF control-plane module
-- Bridges the native Pyxis service (src/rns_bridge.cpp) <-> Lua UI.
--
-- Control-plane only: identity/destination lookup, LXMF send, announce, and
-- the four native-pushed events (message/delivered/announce/status). No
-- audio, no call FSM here — that lands with the LXST work (HYBRID_PLAN M3+).
--
-- Shape mirrors lib/mesh/messages.lua: small in-memory windows, not a full
-- store (the native service + its LXMF MessageStore are the source of
-- truth). Live dispatch from C++ appends to those windows and fires
-- app-registered callbacks. Unlike messages.lua, callback invocation here
-- is pcall-guarded — a bad app callback must not unwind back through the
-- C++ dispatch call that invoked it.

local M = {
    __onMessage   = nil,   -- cb(msg) -- msg = {from=src_hex, text=, timestamp=, dir="in"|"out"}
    __onDelivered = nil,   -- cb(hash_hex, ts)
    __onAnnounce  = nil,   -- cb(entry) -- entry = {dest=hex, name=, last_seen=}
    __onStatus    = nil,   -- cb(text, ts)

    __messages     = {},   -- [src_hex] = { {from=, text=, timestamp=}, ... }, cap 50/src
    __announces    = {},   -- array, insertion order, cap 64
    __announce_idx = {},   -- [dest_hex] = entry (same table as in __announces) for O(1) update
}

local MSG_CAP = 50
local ANNOUNCE_CAP = 64

-- Same trim shape as messages.lua's trim_list: drop from the front so the
-- most recent entries survive; O(n) but only runs once the cap is exceeded.
local function trim_list(list, cap)
    local n = #list
    if n <= cap then return end
    local drop = n - cap
    for i = 1, cap do list[i] = list[i + drop] end
    for i = cap + 1, n do list[i] = nil end
end

-- ── Native accessors ─────────────────────────────────────────────────────

-- True once the service task is up AND has an identity (both can lag a few
-- ticks behind boot).
function M:available()
    return _rns_running() and _rns_identity() ~= nil
end

function M:identity()
    return _rns_identity()
end

function M:dest()
    return _rns_dest()
end

-- Returns hash_hex on success, nil + err on failure (bad dest, send failure).
function M:send(dest_hex, text)
    return _rns_send(dest_hex, text)
end

function M:announce()
    return _rns_announce()
end

-- ── Callback registration ────────────────────────────────────────────────

function M:onMessage(cb)
    M.__onMessage = cb
end

function M:onDelivered(cb)
    M.__onDelivered = cb
end

function M:onAnnounce(cb)
    M.__onAnnounce = cb
end

function M:onStatus(cb)
    M.__onStatus = cb
end

-- ── In-memory windows ────────────────────────────────────────────────────

-- One source's recent messages, oldest first (cap 50).
function M:messages(src_hex)
    return M.__messages[src_hex] or {}
end

-- Record a message WE just sent (optimistic UI): same storage/cap/trim as an
-- incoming one, so M:messages(dest_hex) returns a single interleaved,
-- chronological thread instead of forcing callers to merge two sources.
-- dir differentiates the two ("out" here, "in" from __dispatch_msg below) so
-- a thread view can style/align bubbles without comparing msg.from against
-- our own identity hex. Not native-triggered -- call this yourself right
-- after a successful M:send(); hash (the returned msg_hash_hex) is optional,
-- kept so a later M:onDelivered(hash_hex, ts) can be matched back to this
-- entry. Returns the stored message table.
function M:appendSent(dest_hex, text, ts, hash)
    local msg = { from = _rns_dest(), to = dest_hex, text = text, timestamp = ts, dir = "out", hash = hash }
    local list = M.__messages[dest_hex]
    if not list then
        list = {}
        M.__messages[dest_hex] = list
    end
    table.insert(list, msg)
    trim_list(list, MSG_CAP)
    return msg
end

-- Announce list, oldest-seen-dest first (cap 64). Existing entries update
-- in place (no reordering) so a stable list index is safe across announces.
function M:announces()
    return M.__announces
end

-- ── Dispatch (called from C++ rns_bridge.cpp) ────────────────────────────
-- Plain module functions, no self — C++ calls them via lua_getfield +
-- lua_pcall with positional args only, same convention as
-- lib/mesh/messages.lua's __dispatch/__dispatch_dm/etc.

-- Signature: __dispatch_msg(src_hex, text, ts)
function M.__dispatch_msg(src_hex, text, ts)
    local msg = { from = src_hex, text = text, timestamp = ts, dir = "in" }
    local list = M.__messages[src_hex]
    if not list then
        list = {}
        M.__messages[src_hex] = list
    end
    table.insert(list, msg)
    trim_list(list, MSG_CAP)

    if M.__onMessage then
        local ok, err = pcall(M.__onMessage, msg)
        if not ok then print("[rns] onMessage callback error: " .. tostring(err)) end
    end
end

-- Signature: __dispatch_delivered(hash_hex, ts)
function M.__dispatch_delivered(hash_hex, ts)
    if M.__onDelivered then
        local ok, err = pcall(M.__onDelivered, hash_hex, ts)
        if not ok then print("[rns] onDelivered callback error: " .. tostring(err)) end
    end
end

-- Signature: __dispatch_announce(dest_hex, name, ts)
function M.__dispatch_announce(dest_hex, name, ts)
    local entry = M.__announce_idx[dest_hex]
    if entry then
        entry.name = name
        entry.last_seen = ts
    else
        entry = { dest = dest_hex, name = name, last_seen = ts }
        M.__announce_idx[dest_hex] = entry
        table.insert(M.__announces, entry)
        if #M.__announces > ANNOUNCE_CAP then
            local dropped = table.remove(M.__announces, 1)
            M.__announce_idx[dropped.dest] = nil
        end
    end

    if M.__onAnnounce then
        local ok, err = pcall(M.__onAnnounce, entry)
        if not ok then print("[rns] onAnnounce callback error: " .. tostring(err)) end
    end
end

-- Signature: __dispatch_status(text, ts)
function M.__dispatch_status(text, ts)
    if M.__onStatus then
        local ok, err = pcall(M.__onStatus, text, ts)
        if not ok then print("[rns] onStatus callback error: " .. tostring(err)) end
    end
end

-- Display name carried in our LXMF announces ("" when unset — peers
-- see the hash). setName persists to NVS and re-announces immediately.
function M:getName()
    local ok, name = pcall(_rns_get_name)
    return (ok and name) or ""
end

function M:setName(name)
    local ok, res = pcall(_rns_set_name, tostring(name or ""))
    return ok and res or false
end

-- Optional TCP client interface (off by default; AutoInterface is the
-- primary path and needs no config). getTcp returns a table; setTcp
-- persists — enabling with WiFi up connects immediately, disabling
-- applies at next boot.
function M:getTcp()
    local ok, en, host, port, online = pcall(_rns_get_tcp)
    if not ok then return { enabled = false, host = "", port = 4965, online = false } end
    return { enabled = en, host = host or "", port = port or 4965, online = online }
end

-- AutoInterface (IPv6 multicast peer discovery). Optional since
-- 2026-07-23: congested 2.4GHz networks flap its carrier. setAuto
-- persists; takes effect at next boot.
function M:getAuto()
    local ok, en, running = pcall(_rns_get_auto)
    if not ok then return { enabled = true, running = false } end
    return { enabled = en, running = running }
end

function M:setAuto(enabled)
    local ok, res = pcall(_rns_set_auto, enabled and true or false)
    return ok and res or false
end

function M:setTcp(enabled, host, port)
    local ok, res = pcall(_rns_set_tcp, enabled and true or false,
                          tostring(host or ""), tonumber(port) or 4965)
    return ok and res or false
end

return M
