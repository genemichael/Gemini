// Control-plane Lua binding for the Pyxis LXST voice-call engine.
//
// Lua -> native (flat globals, underscore-prefixed, exactly like
// rns_bridge.cpp / fs_bridge.cpp — never luaL_newlib/luaL_requiref, that's
// reserved for luavgl's `lvgl` module):
//
//   _phone_dial(dest_hex)   -> bool
//   _phone_answer()         -> bool
//   _phone_hangup()         -> bool
//   _phone_state()          -> string ("IDLE", "RINGING", "ACTIVE", ...)
//   _phone_duration()       -> integer seconds
//   _phone_peer()           -> hex string | "" (no current/last-known peer)
//   _phone_mute(bool)       -> bool
//   _phone_muted()          -> bool
//
// Native -> Lua: phone_bridge_dispatch() pushes one PyxisEvent into
// lib/phone.lua's __dispatch_* functions, following rns_bridge.cpp's
// rns_push_*/lua_mesh_push_* pattern exactly — require the module,
// lua_getfield the dispatch function, lua_pcall with the right arg count,
// pop the module table off. Every step is guarded (missing module /
// non-function field / pcall error all just return); nothing here may
// longjmp out, since this runs on drain_rns_events()'s core-0 loop-tick path.

#include "phone_bridge.h"
#include "PyxisCall.h"
#include "meshpunk_sync.h"   // SLog

#include <ctype.h>
#include <string.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

// dest_hex must be exactly 32 hex chars (16-byte Reticulum destination hash)
// before it's handed to pyxis_call_initiate — reject garbage here rather than
// let the call engine decide what "malformed" means. Same shape as
// rns_bridge.cpp's is_hex32 (duplicated, not shared, to keep each bridge
// self-contained — matches the project's existing per-file pattern).
static bool is_hex32(const char* s) {
    if (!s) return false;
    size_t n = strlen(s);
    if (n != 32) return false;
    for (size_t i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

// _phone_dial(dest_hex) -> bool
static int lua_phone_dial(lua_State* L) {
    const char* dest_hex = luaL_checkstring(L, 1);
    if (!is_hex32(dest_hex)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, pyxis_call_initiate(dest_hex) ? 1 : 0);
    return 1;
}

// _phone_answer() -> bool
static int lua_phone_answer(lua_State* L) {
    lua_pushboolean(L, pyxis_call_answer() ? 1 : 0);
    return 1;
}

// _phone_hangup() -> bool
static int lua_phone_hangup(lua_State* L) {
    lua_pushboolean(L, pyxis_call_hangup() ? 1 : 0);
    return 1;
}

// _phone_state() -> string
static int lua_phone_state(lua_State* L) {
    lua_pushstring(L, pyxis_call_state_name());
    return 1;
}

// _phone_duration() -> integer seconds
static int lua_phone_duration(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)pyxis_call_duration_s());
    return 1;
}

// _phone_peer() -> hex string | "" (pyxis_call_peer always NUL-terminates,
// empty string when there's no current/last-known peer).
static int lua_phone_peer(lua_State* L) {
    char hex[33];
    pyxis_call_peer(hex);
    lua_pushstring(L, hex);
    return 1;
}

// _phone_mute(bool) -> bool
static int lua_phone_mute(lua_State* L) {
    bool muted = lua_toboolean(L, 1);
    lua_pushboolean(L, pyxis_call_set_mute(muted) ? 1 : 0);
    return 1;
}

// _phone_muted() -> bool
static int lua_phone_muted(lua_State* L) {
    lua_pushboolean(L, pyxis_call_muted() ? 1 : 0);
    return 1;
}

void phone_bridge_register(lua_State* L) {
    lua_register(L, "_phone_dial",     lua_phone_dial);
    lua_register(L, "_phone_answer",   lua_phone_answer);
    lua_register(L, "_phone_hangup",   lua_phone_hangup);
    lua_register(L, "_phone_state",    lua_phone_state);
    lua_register(L, "_phone_duration", lua_phone_duration);
    lua_register(L, "_phone_peer",     lua_phone_peer);
    lua_register(L, "_phone_mute",     lua_phone_mute);
    lua_register(L, "_phone_muted",    lua_phone_muted);
}

// ── C++ -> Lua dispatch (one function per PyxisEvent kind) ─────────────────
// Each mirrors rns_push_*'s shape verbatim: require "lib/phone", getfield the
// __dispatch_* function, push args, pcall, pop the module table. Strings come
// straight from PyxisEvent's fixed, NUL-terminated buffers (owned by the
// caller's stack copy in drain_rns_events()), so lua_pushstring is safe as-is.

static void phone_push_incoming(lua_State* L, const PyxisEvent& ev) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/phone");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        SLog.printf("[phone] require failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "__dispatch_incoming");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    lua_pushstring(L, ev.peer_hash);   // arg1: peer_hex (caller identity hash)
    lua_pushinteger(L, ev.ts);         // arg2: ts
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        SLog.printf("[phone] __dispatch_incoming failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);   // module table
}

static void phone_push_call_state(lua_State* L, const PyxisEvent& ev) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/phone");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        SLog.printf("[phone] require failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "__dispatch_call_state");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    lua_pushstring(L, ev.text);        // arg1: state_name
    lua_pushstring(L, ev.peer_hash);   // arg2: peer_hex
    lua_pushinteger(L, ev.ts);         // arg3: ts
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        SLog.printf("[phone] __dispatch_call_state failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static void phone_push_missed(lua_State* L, const PyxisEvent& ev) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/phone");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        SLog.printf("[phone] require failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "__dispatch_missed");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    lua_pushstring(L, ev.peer_hash);   // arg1: peer_hex (caller identity hash)
    lua_pushinteger(L, ev.ts);         // arg2: ts
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        SLog.printf("[phone] __dispatch_missed failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

void phone_bridge_dispatch(lua_State* L, const PyxisEvent& ev) {
    if (!L) return;   // VM torn down (ELF run in progress) — caller should
                       // already guard this, but never assume.
    switch (ev.kind) {
        case PyxisEvent::CALL_INCOMING: phone_push_incoming(L, ev);   break;
        case PyxisEvent::CALL_STATE:    phone_push_call_state(L, ev); break;
        case PyxisEvent::MISSED_CALL:   phone_push_missed(L, ev);     break;
        default: break;   // MSG_RECEIVED etc. — owned by rns_bridge_dispatch
    }
}
