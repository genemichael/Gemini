--[[
  contacts — the unified Address Book store: one entry per PERSON, linking
  their radio (MeshCore) and WiFi (Reticulum/LXMF) identities.

  Persisted as JSON at /lua/contacts.json (LittleFS, via lib/fileman -- the
  same C _fs_* bridge every other on-device store already goes through).
  No native/lib call in this module is trusted: every one is pcall-wrapped,
  and a missing or corrupt store file just starts empty -- never crash the
  launcher over a bad file.

  Save discipline: synchronous, on every mutation. A debounced/timer-based
  save is not available to a *lib* (apps.add_timer is scoped to the current
  foreground app and torn down when it exits; a lib has no lifecycle hook to
  register one against) -- and unnecessary here anyway, since the store is a
  handful of small entries, not a hot path.

  Entry shape:
    { id, name,
      mesh_pubkey,   -- 64 hex chars (32-byte MeshCore public key), optional
      rns_identity,  -- 32 hex chars (16-byte RNS identity hash), optional
      rns_lxmf,      -- 32 hex chars (16-byte LXMF delivery dest), optional
      rns_lxst,      -- 32 hex chars (16-byte LXST voice dest), optional
      created, updated }   -- RTC unix seconds

  API (colon-method style, matching lib/rns.lua):
    load() list() get(id) add(fields) update(id, fields) remove(id)
    find_by_mesh(pubkey) find_by_lxmf(dest_hex)
    link_mesh(id, pubkey) link_lxmf(id, dest_hex)
    unlink_mesh(id) unlink_lxmf(id)
    candidates_from_mesh() candidates_from_rns()

  KNOWN GAP (see the Contacts app / hybrid-phone report for detail): the
  native rns_bridge.cpp surface (src/rns_bridge.cpp) exposes exactly ONE
  announce kind -- __dispatch_announce(dest_hex, name, ts) -- with no field
  distinguishing an RNS identity hash from an LXMF delivery-destination hash
  from an LXST voice-destination hash, and lib/rns.lua's announces() (and
  the RNS app, which sends straight to announces()[i].dest) already treats
  that dest_hex as the LXMF destination. So candidates_from_rns() below can
  only ever populate rns_lxmf; rns_identity/rns_lxst are schema fields for
  forward compatibility with a future richer announce event, not something
  this module (or the Contacts app) can populate today. Do not "fix" this
  by guessing at a mapping -- it needs a real native-side change, out of
  scope for a Lua-only task (rns_bridge.cpp is off-limits here regardless).
]]

local fileman = require("lib/fileman")
local json = require("lib/json")

local M = {
    __loaded = false,
    __entries = {},   -- array, insertion order
    __index = {},      -- [id] = entry
    __by_mesh = {},    -- [mesh_pubkey] = entry
    __by_lxmf = {},    -- [rns_lxmf] = entry
    __next_id = 1,
}

local PATH = "/lua/contacts.json"

-- ── Helpers (locals; declared before first use -- see ordering note below:
-- a `local function` must be lexically declared before any OTHER top-level
-- function body that references it, even though the actual CALL happens
-- later at runtime. M:method bodies referencing M.__fields or other M:
-- methods have no such constraint -- table-field lookups are dynamic.) ────

local function is_hex(s, len)
    return type(s) == "string" and #s == len and s:match("^%x+$") ~= nil
end

-- Device RTC epoch, same convention as lib/mesh/messages.lua's now_ts()
-- (os.time() is not synced to the RTC on this firmware).
local function now_ts()
    local ok, t = pcall(_rtc_time)
    if ok and t and t > 0 then return t end
    return os.time()
end

local function rebuild_indices()
    M.__index, M.__by_mesh, M.__by_lxmf = {}, {}, {}
    for _, e in ipairs(M.__entries) do
        M.__index[e.id] = e
        if type(e.mesh_pubkey) == "string" and e.mesh_pubkey ~= "" then
            M.__by_mesh[e.mesh_pubkey] = e
        end
        if type(e.rns_lxmf) == "string" and e.rns_lxmf ~= "" then
            M.__by_lxmf[e.rns_lxmf] = e
        end
    end
end

-- Validate + lowercase the optional identity fields in `fields`
-- (mesh_pubkey 64-hex; rns_identity/rns_lxmf/rns_lxst 32-hex each), and
-- reject a mesh_pubkey/rns_lxmf that's already linked to a DIFFERENT entry
-- (rns_identity/rns_lxst have no index -- nothing populates them yet, see
-- the header gap note -- so only a format check applies to those two).
-- self_id (nil for a brand new entry) exempts that entry's own current
-- value from the collision check. Returns a table of just the validated
-- fields present in the input, or nil, err.
local function validate_fields(fields, self_id)
    local out = {}
    if fields.mesh_pubkey ~= nil then
        local pk = tostring(fields.mesh_pubkey):lower()
        if not is_hex(pk, 64) then return nil, "mesh pubkey must be 64 hex chars" end
        local existing = M.__by_mesh[pk]
        if existing and existing.id ~= self_id then
            return nil, "mesh identity already linked to " .. (existing.name or existing.id)
        end
        out.mesh_pubkey = pk
    end
    if fields.rns_lxmf ~= nil then
        local d = tostring(fields.rns_lxmf):lower()
        if not is_hex(d, 32) then return nil, "rns lxmf hash must be 32 hex chars" end
        local existing = M.__by_lxmf[d]
        if existing and existing.id ~= self_id then
            return nil, "rns identity already linked to " .. (existing.name or existing.id)
        end
        out.rns_lxmf = d
    end
    if fields.rns_identity ~= nil then
        local d = tostring(fields.rns_identity):lower()
        if not is_hex(d, 32) then return nil, "rns identity hash must be 32 hex chars" end
        out.rns_identity = d
    end
    if fields.rns_lxst ~= nil then
        local d = tostring(fields.rns_lxst):lower()
        if not is_hex(d, 32) then return nil, "rns lxst hash must be 32 hex chars" end
        out.rns_lxst = d
    end
    return out
end

local function save()
    pcall(fileman.mkdir, "/lua")   -- belt: parent should already exist
    local root = { next_id = M.__next_id, entries = M.__entries }
    local ok_enc, body = pcall(json.encode, root)
    if not ok_enc then
        print("[contacts] encode failed: " .. tostring(body))
        return false
    end
    local ok_call, ok_write, werr = pcall(fileman.write, PATH, body)
    if not (ok_call and ok_write) then
        print("[contacts] save failed: " .. tostring(werr or ok_write))
        return false
    end
    return true
end

local function ensure_loaded()
    if not M.__loaded then M:load() end
end

-- ── Public API ───────────────────────────────────────────────────────────

-- (Re)load from disk, discarding any in-memory state. Missing file, empty
-- file, corrupt JSON, or a non-table root all just leave the store empty --
-- never an error the caller has to handle.
function M:load()
    M.__entries, M.__next_id, M.__loaded = {}, 1, true

    local ok_read, data = pcall(fileman.read, PATH)
    if ok_read and type(data) == "string" and data ~= "" then
        local ok_dec, root = pcall(json.decode, data)
        if ok_dec and type(root) == "table" then
            if type(root.entries) == "table" then
                -- Keep only well-formed rows -- one bad/hand-edited entry
                -- must not sink the whole store.
                local kept = {}
                for _, e in ipairs(root.entries) do
                    if type(e) == "table" and type(e.id) == "string"
                        and type(e.name) == "string" then
                        kept[#kept + 1] = e
                    end
                end
                M.__entries = kept
            end
            local n = tonumber(root.next_id)
            if n and n >= 1 then M.__next_id = math.floor(n) end
        end
        -- decode failure / non-table root: fall through with the empty
        -- defaults already set above.
    end

    -- Guard a stale or hand-edited next_id against existing "cNN" ids so
    -- add() can never mint a duplicate.
    for _, e in ipairs(M.__entries) do
        local n = tonumber(e.id:match("^c(%d+)$"))
        if n and n >= M.__next_id then M.__next_id = n + 1 end
    end

    rebuild_indices()
    return M.__entries
end

-- Live array, insertion order (oldest-added first). Callers sort/display
-- as they like.
function M:list()
    ensure_loaded()
    return M.__entries
end

function M:get(id)
    ensure_loaded()
    return M.__index[id]
end

-- fields = { name, mesh_pubkey?, rns_identity?, rns_lxmf?, rns_lxst? }.
-- name is required (trimmed, must be non-empty); identity fields are
-- optional and validated/dedup-checked via validate_fields. Returns the
-- new entry, or nil, err on validation failure.
function M:add(fields)
    ensure_loaded()
    fields = fields or {}
    local name = tostring(fields.name or ""):match("^%s*(.-)%s*$")
    if name == "" then return nil, "name is required" end
    local ident, err = validate_fields(fields, nil)
    if not ident then return nil, err end

    local ts = now_ts()
    local entry = {
        id = "c" .. M.__next_id,
        name = name,
        mesh_pubkey = ident.mesh_pubkey,
        rns_identity = ident.rns_identity,
        rns_lxmf = ident.rns_lxmf,
        rns_lxst = ident.rns_lxst,
        created = ts,
        updated = ts,
    }
    M.__next_id = M.__next_id + 1
    table.insert(M.__entries, entry)
    M.__index[entry.id] = entry
    if entry.mesh_pubkey then M.__by_mesh[entry.mesh_pubkey] = entry end
    if entry.rns_lxmf then M.__by_lxmf[entry.rns_lxmf] = entry end
    save()
    return entry
end

-- fields = any of { name, mesh_pubkey, rns_identity, rns_lxmf, rns_lxst }.
-- Only the keys present are changed. Returns true, entry | false, err.
function M:update(id, fields)
    ensure_loaded()
    local entry = M.__index[id]
    if not entry then return false, "no such contact" end
    fields = fields or {}

    local new_name
    if fields.name ~= nil then
        new_name = tostring(fields.name):match("^%s*(.-)%s*$")
        if new_name == "" then return false, "name is required" end
    end
    local ident, err = validate_fields(fields, id)
    if not ident then return false, err end

    if new_name then entry.name = new_name end
    if ident.mesh_pubkey then
        if entry.mesh_pubkey then M.__by_mesh[entry.mesh_pubkey] = nil end
        entry.mesh_pubkey = ident.mesh_pubkey
        M.__by_mesh[entry.mesh_pubkey] = entry
    end
    if ident.rns_lxmf then
        if entry.rns_lxmf then M.__by_lxmf[entry.rns_lxmf] = nil end
        entry.rns_lxmf = ident.rns_lxmf
        M.__by_lxmf[entry.rns_lxmf] = entry
    end
    if ident.rns_identity then entry.rns_identity = ident.rns_identity end
    if ident.rns_lxst then entry.rns_lxst = ident.rns_lxst end
    entry.updated = now_ts()
    save()
    return true, entry
end

function M:remove(id)
    ensure_loaded()
    local entry = M.__index[id]
    if not entry then return false end
    for i, e in ipairs(M.__entries) do
        if e.id == id then
            table.remove(M.__entries, i)
            break
        end
    end
    M.__index[id] = nil
    if entry.mesh_pubkey then M.__by_mesh[entry.mesh_pubkey] = nil end
    if entry.rns_lxmf then M.__by_lxmf[entry.rns_lxmf] = nil end
    save()
    return true
end

function M:find_by_mesh(pubkey)
    ensure_loaded()
    return M.__by_mesh[tostring(pubkey or ""):lower()]
end

function M:find_by_lxmf(dest_hex)
    ensure_loaded()
    return M.__by_lxmf[tostring(dest_hex or ""):lower()]
end

-- Thin wrappers over update() -- kept as named verbs because the Contacts
-- app's detail screen and the import pickers call these specifically.
function M:link_mesh(id, pubkey)
    return M:update(id, { mesh_pubkey = pubkey })
end

function M:link_lxmf(id, dest_hex)
    return M:update(id, { rns_lxmf = dest_hex })
end

function M:unlink_mesh(id)
    ensure_loaded()
    local entry = M.__index[id]
    if not entry then return false, "no such contact" end
    if not entry.mesh_pubkey then return false, "not linked" end
    M.__by_mesh[entry.mesh_pubkey] = nil
    entry.mesh_pubkey = nil
    entry.updated = now_ts()
    save()
    return true
end

function M:unlink_lxmf(id)
    ensure_loaded()
    local entry = M.__index[id]
    if not entry then return false, "no such contact" end
    if not entry.rns_lxmf then return false, "not linked" end
    M.__by_lxmf[entry.rns_lxmf] = nil
    entry.rns_lxmf = nil
    entry.updated = now_ts()
    save()
    return true
end

-- MeshCore contacts (lib/mesh/messages.lua's M:getContacts(), itself a thin
-- wrap of _mesh_get_contacts()) not yet linked to any Address Book entry.
-- Entries: { origin="mesh", name, mesh_pubkey, type_name, last_seen }.
-- pcall-guarded at every native/lib boundary; mesh unavailable (module
-- missing, native call errors, non-table result) just yields {}.
function M:candidates_from_mesh()
    ensure_loaded()
    local ok_req, messages = pcall(require, "lib/mesh/messages")
    if not ok_req or not messages then return {} end
    local ok_list, list = pcall(function() return messages:getContacts() end)
    if not ok_list or type(list) ~= "table" then return {} end

    local out = {}
    for _, c in ipairs(list) do
        local pk = type(c.pubkey) == "string" and c.pubkey:lower() or nil
        if pk and is_hex(pk, 64) and not M.__by_mesh[pk] then
            out[#out + 1] = {
                origin = "mesh",
                name = c.name or "",
                mesh_pubkey = pk,
                type_name = c.type_name,
                last_seen = c.last_seen,
            }
        end
    end
    return out
end

-- RNS announces (lib/rns.lua's M:announces(), in-memory window fed by
-- native ANNOUNCE events) not yet linked to any Address Book entry.
-- Entries: { origin="rns", name, rns_lxmf, last_seen }. See the header gap
-- note: only rns_lxmf is derivable from the current announce event shape.
function M:candidates_from_rns()
    ensure_loaded()
    local ok_req, rns = pcall(require, "lib/rns")
    if not ok_req or not rns then return {} end
    local ok_list, list = pcall(function() return rns:announces() end)
    if not ok_list or type(list) ~= "table" then return {} end

    local out = {}
    for _, e in ipairs(list) do
        local dest = type(e.dest) == "string" and e.dest:lower() or nil
        if dest and is_hex(dest, 32) and not M.__by_lxmf[dest] then
            out[#out + 1] = {
                origin = "rns",
                name = e.name or "",
                rns_lxmf = dest,
                last_seen = e.last_seen,
            }
        end
    end
    return out
end

return M
