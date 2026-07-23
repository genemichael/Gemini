--[[
  json — minimal pure-Lua JSON encode/decode.

  No JSON mechanism exists anywhere in this firmware today (verified: no
  _json_* native globals anywhere in src/, no "json"/"JSON" hit anywhere
  under data/lua/). The on-disk catalog format used by lib/downloader.lua
  (App Library / Theme store) is TOML via lib/toml.lua, not JSON, and
  nothing else on-device parses structured config. Written for
  lib/contacts.lua's /lua/contacts.json store; deliberately small — it
  encodes/decodes plain Lua tables (objects: string-keyed; arrays: dense
  1..#t), strings, numbers, booleans and null. It is NOT a fuzzed
  general-purpose parser: callers own file-corruption handling (contacts.lua
  treats any decode failure as "start empty", per its header).

  M.encode(value)   -> json string (never errors on ordinary Lua data --
                        see encode_value's fallback branch)
  M.decode(str)      -> value, nil on success | nil, err on failure (never
                        throws -- internally pcall'd)
  M.null             -> sentinel written/read back for a literal JSON null
                        (a plain Lua `nil` inside a table has no distinct
                        identity from "key absent" and is always just
                        skipped by the encoder)

  Known limitation: Lua cannot represent an empty JSON array distinctly
  from an empty JSON object (both are just `{}`) -- an empty Lua table
  always encodes as `{}`. Harmless for a reader that always iterates a
  known field with ipairs()/pairs() (ipairs on `{}` is a zero-iteration
  no-op either way), which is the only way this module's output is ever
  consumed here.
]]

local M = {}

-- Sentinel for a JSON `null` that round-trips through decode.
M.null = setmetatable({}, { __tostring = function() return "null" end })

-- ── Encode ───────────────────────────────────────────────────────────────

local escapes = {
    ["\""] = "\\\"", ["\\"] = "\\\\", ["\b"] = "\\b", ["\f"] = "\\f",
    ["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t",
}

local function encode_string(s)
    local out = { "\"" }
    for i = 1, #s do
        local c = s:sub(i, i)
        local e = escapes[c]
        if e then
            out[#out + 1] = e
        elseif c:byte() < 0x20 then
            out[#out + 1] = string.format("\\u%04x", c:byte())
        else
            out[#out + 1] = c
        end
    end
    out[#out + 1] = "\""
    return table.concat(out)
end

-- A table is an "array" iff every key is exactly 1..n for its key count n
-- (n >= 1) -- otherwise (mixed keys, non-sequential, or empty) it encodes
-- as an object. table.insert-built entries (contacts.lua's shape) always
-- satisfy this.
local function is_array(t)
    local n = 0
    for _ in pairs(t) do n = n + 1 end
    if n == 0 then return false end
    for i = 1, n do
        if t[i] == nil then return false end
    end
    return true
end

local encode_value

local function encode_array(t)
    local parts = {}
    for i = 1, #t do parts[i] = encode_value(t[i]) end
    return "[" .. table.concat(parts, ",") .. "]"
end

local function encode_object(t)
    local parts = {}
    for k, v in pairs(t) do
        if type(k) == "string" then
            parts[#parts + 1] = encode_string(k) .. ":" .. encode_value(v)
        end
    end
    return "{" .. table.concat(parts, ",") .. "}"
end

encode_value = function(v)
    if v == M.null then return "null" end
    local t = type(v)
    if t == "string" then
        return encode_string(v)
    elseif t == "number" then
        if v ~= v or v == math.huge or v == -math.huge then return "null" end
        if math.floor(v) == v and math.abs(v) < 1e15 then
            return string.format("%d", v)
        end
        return tostring(v)
    elseif t == "boolean" then
        return v and "true" or "false"
    elseif t == "table" then
        if is_array(v) then return encode_array(v) else return encode_object(v) end
    else
        -- nil / function / userdata: never expected as a value reachable
        -- from contacts.lua's data; fall back to null rather than error.
        return "null"
    end
end

function M.encode(value)
    return encode_value(value)
end

-- ── Decode ───────────────────────────────────────────────────────────────
-- Recursive-descent over the string. Small inputs only (a config/store
-- file, not a stream). Internal errors use error(), caught once at the
-- M.decode boundary so callers never need their own pcall.

local function skip_ws(s, i)
    local _, e = s:find("^[ \t\r\n]*", i)
    return e + 1
end

local function perr(i, msg)
    error(string.format("json: %s at byte %d", msg, i), 0)
end

local parse_value

local function parse_string(s, i)
    -- s:sub(i,i) is the opening quote.
    local j = i + 1
    local out = {}
    while true do
        local c = s:sub(j, j)
        if c == "" then perr(j, "unterminated string") end
        if c == "\"" then
            return table.concat(out), j + 1
        elseif c == "\\" then
            local e = s:sub(j + 1, j + 1)
            if e == "\"" then out[#out + 1] = "\""; j = j + 2
            elseif e == "\\" then out[#out + 1] = "\\"; j = j + 2
            elseif e == "/" then out[#out + 1] = "/"; j = j + 2
            elseif e == "b" then out[#out + 1] = "\b"; j = j + 2
            elseif e == "f" then out[#out + 1] = "\f"; j = j + 2
            elseif e == "n" then out[#out + 1] = "\n"; j = j + 2
            elseif e == "r" then out[#out + 1] = "\r"; j = j + 2
            elseif e == "t" then out[#out + 1] = "\t"; j = j + 2
            elseif e == "u" then
                local hex = s:sub(j + 2, j + 5)
                local cp = tonumber(hex, 16)
                if not cp then perr(j, "bad \\u escape") end
                -- BMP-only (no surrogate-pair joining): sufficient for the
                -- ASCII/Latin display names this store actually carries. A
                -- lone surrogate half falls back to '?' rather than failing
                -- the whole decode.
                if cp < 0x80 then
                    out[#out + 1] = string.char(cp)
                elseif cp < 0x800 then
                    out[#out + 1] = string.char(0xC0 + math.floor(cp / 64), 0x80 + cp % 64)
                elseif cp >= 0xD800 and cp <= 0xDFFF then
                    out[#out + 1] = "?"
                else
                    out[#out + 1] = string.char(
                        0xE0 + math.floor(cp / 4096),
                        0x80 + math.floor(cp / 64) % 64,
                        0x80 + cp % 64)
                end
                j = j + 6
            else
                perr(j, "bad escape")
            end
        else
            out[#out + 1] = c
            j = j + 1
        end
    end
end

local function parse_number(s, i)
    local m = s:match("^%-?%d+%.?%d*[eE]?[%+%-]?%d*", i)
    local n = m and tonumber(m)
    if not n then perr(i, "bad number") end
    return n, i + #m
end

parse_value = function(s, i)
    i = skip_ws(s, i)
    local c = s:sub(i, i)
    if c == "\"" then
        return parse_string(s, i)
    elseif c == "{" then
        local obj = {}
        i = skip_ws(s, i + 1)
        if s:sub(i, i) == "}" then return obj, i + 1 end
        while true do
            i = skip_ws(s, i)
            if s:sub(i, i) ~= "\"" then perr(i, "expected string key") end
            local key
            key, i = parse_string(s, i)
            i = skip_ws(s, i)
            if s:sub(i, i) ~= ":" then perr(i, "expected ':'") end
            local val
            val, i = parse_value(s, i + 1)
            obj[key] = val
            i = skip_ws(s, i)
            local d = s:sub(i, i)
            if d == "," then i = i + 1
            elseif d == "}" then return obj, i + 1
            else perr(i, "expected ',' or '}'") end
        end
    elseif c == "[" then
        local arr = {}
        i = skip_ws(s, i + 1)
        if s:sub(i, i) == "]" then return arr, i + 1 end
        local n = 0
        while true do
            local val
            val, i = parse_value(s, i)
            n = n + 1
            arr[n] = val
            i = skip_ws(s, i)
            local d = s:sub(i, i)
            if d == "," then i = i + 1
            elseif d == "]" then return arr, i + 1
            else perr(i, "expected ',' or ']'") end
        end
    elseif s:sub(i, i + 3) == "true" then
        return true, i + 4
    elseif s:sub(i, i + 4) == "false" then
        return false, i + 5
    elseif s:sub(i, i + 3) == "null" then
        return M.null, i + 4
    elseif c:match("[%-%d]") then
        return parse_number(s, i)
    else
        perr(i, "unexpected character '" .. c .. "'")
    end
end

-- Never throws: internal parse errors are caught here and returned as
-- nil, err. Empty/non-string input is rejected the same way.
function M.decode(s)
    if type(s) ~= "string" or s == "" then return nil, "empty input" end
    local ok, result = pcall(function()
        local v, i = parse_value(s, 1)
        i = skip_ws(s, i)
        if i <= #s then perr(i, "trailing data") end
        return v
    end)
    if ok then return result, nil end
    return nil, result
end

return M
