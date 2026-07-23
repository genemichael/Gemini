// Control-plane Lua binding for the Pyxis RNS/LXMF service.
//
// Lua -> native (flat globals, underscore-prefixed, exactly like
// fs_bridge.cpp — never luaL_newlib/luaL_requiref, that's reserved for
// luavgl's `lvgl` module):
//
//   _rns_running()             -> bool
//   _rns_identity()            -> hex string | nil
//   _rns_dest()                -> hex string | nil
//   _rns_send(dest_hex, text)  -> msg_hash_hex | nil, err
//   _rns_announce()            -> bool
//   _rns_get_name()            -> string ("" when unset)
//   _rns_set_name(name)        -> bool (persists + re-announces)
//
// Native -> Lua: rns_bridge_dispatch() pushes one PyxisEvent into
// lib/rns.lua's __dispatch_* functions, following punkmesh.cpp's
// lua_mesh_push_* pattern exactly — require the module, lua_getfield the
// dispatch function, lua_pcall with the right arg count, pop the module
// table off. Every step is guarded (missing module / non-function field /
// pcall error all just return); nothing here may longjmp out, since this
// runs on drain_rns_events()'s core-0 loop-tick path.

#include "rns_bridge.h"
#include "meshpunk_sync.h"   // SLog

#include <ctype.h>
#include <string.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

// dest_hex must be exactly 32 hex chars (16-byte Reticulum destination
// hash) before it's handed to pyxis_send_lxmf — reject garbage here rather
// than let the service layer decide what "malformed" means.
static bool is_hex32(const char* s) {
    if (!s) return false;
    size_t n = strlen(s);
    if (n != 32) return false;
    for (size_t i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

// _rns_running() -> bool
static int lua_rns_running(lua_State* L) {
    lua_pushboolean(L, pyxis_service_running() ? 1 : 0);
    return 1;
}

// _rns_identity() -> hex string | nil (nil until the service has an
// identity, e.g. still starting up).
static int lua_rns_identity(lua_State* L) {
    char hex[33];
    if (!pyxis_get_identity_hash(hex)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, hex);
    return 1;
}

// _rns_dest() -> hex string | nil (the LXMF delivery destination hash).
static int lua_rns_dest(lua_State* L) {
    char hex[33];
    if (!pyxis_get_delivery_dest(hex)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, hex);
    return 1;
}

// _rns_send(dest_hex, text) -> msg_hash_hex | nil, err
static int lua_rns_send(lua_State* L) {
    const char* dest_hex = luaL_checkstring(L, 1);
    const char* text = luaL_checkstring(L, 2);
    if (!is_hex32(dest_hex)) {
        lua_pushnil(L);
        lua_pushstring(L, "dest must be 32 hex chars");
        return 2;
    }
    char out_hash[33];
    if (!pyxis_send_lxmf(dest_hex, text, out_hash)) {
        lua_pushnil(L);
        lua_pushstring(L, "send failed");
        return 2;
    }
    lua_pushstring(L, out_hash);
    return 1;
}

// _rns_announce() -> bool
static int lua_rns_announce(lua_State* L) {
    lua_pushboolean(L, pyxis_announce() ? 1 : 0);
    return 1;
}

// _rns_get_name() -> string ("" when unset)
static int lua_rns_get_name(lua_State* L) {
    char name[32] = {0};
    pyxis_get_display_name(name);
    lua_pushstring(L, name);
    return 1;
}

// _rns_set_name(name) -> bool. Persists to NVS, applies to the router
// live, and re-announces so peers pick the new name up.
static int lua_rns_set_name(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_pushboolean(L, pyxis_set_display_name(name) ? 1 : 0);
    return 1;
}

// _rns_get_tcp() -> enabled(bool), host(string), port(int), online(bool)
static int lua_rns_get_tcp(lua_State* L) {
    bool en = false, online = false;
    char host[64] = {0};
    uint16_t port = 0;
    pyxis_get_tcp(&en, host, &port, &online);
    lua_pushboolean(L, en ? 1 : 0);
    lua_pushstring(L, host);
    lua_pushinteger(L, port);
    lua_pushboolean(L, online ? 1 : 0);
    return 4;
}

// _rns_set_tcp(enabled, host, port) -> bool. Persists; enabling with
// WiFi up connects immediately; disabling applies at next boot.
static int lua_rns_set_tcp(lua_State* L) {
    bool en = lua_toboolean(L, 1);
    const char* host = luaL_optstring(L, 2, "");
    int port = (int)luaL_optinteger(L, 3, 4965);
    if (port < 0 || port > 65535) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, pyxis_set_tcp(en, host, (uint16_t)port) ? 1 : 0);
    return 1;
}

void rns_bridge_register(lua_State* L) {
    lua_register(L, "_rns_running",  lua_rns_running);
    lua_register(L, "_rns_identity", lua_rns_identity);
    lua_register(L, "_rns_dest",     lua_rns_dest);
    lua_register(L, "_rns_send",     lua_rns_send);
    lua_register(L, "_rns_announce", lua_rns_announce);
    lua_register(L, "_rns_get_name", lua_rns_get_name);
    lua_register(L, "_rns_set_name", lua_rns_set_name);
    lua_register(L, "_rns_get_tcp",  lua_rns_get_tcp);
    lua_register(L, "_rns_set_tcp",  lua_rns_set_tcp);
}

// ── C++ -> Lua dispatch (one function per PyxisEvent kind) ─────────────────
// Each mirrors lua_mesh_push_*'s shape verbatim: require "lib/rns", getfield
// the __dispatch_* function, push args, pcall, pop the module table. Strings
// come straight from PyxisEvent's fixed, NUL-terminated buffers (owned by the
// caller's stack copy in drain_rns_events()), so lua_pushstring is safe as-is.

static void rns_push_msg(lua_State* L, const PyxisEvent& ev) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/rns");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        SLog.printf("[rns] require failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "__dispatch_msg");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    lua_pushstring(L, ev.peer_hash);   // arg1: src_hex
    lua_pushstring(L, ev.text);        // arg2: text
    lua_pushinteger(L, ev.ts);         // arg3: ts
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        SLog.printf("[rns] __dispatch_msg failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);   // module table
}

static void rns_push_delivered(lua_State* L, const PyxisEvent& ev) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/rns");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        SLog.printf("[rns] require failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "__dispatch_delivered");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    lua_pushstring(L, ev.peer_hash);   // arg1: hash_hex (message hash)
    lua_pushinteger(L, ev.ts);         // arg2: ts
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        SLog.printf("[rns] __dispatch_delivered failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static void rns_push_announce(lua_State* L, const PyxisEvent& ev) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/rns");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        SLog.printf("[rns] require failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "__dispatch_announce");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    lua_pushstring(L, ev.peer_hash);   // arg1: dest_hex
    lua_pushstring(L, ev.peer_name);   // arg2: name (may be "")
    lua_pushinteger(L, ev.ts);         // arg3: ts
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        SLog.printf("[rns] __dispatch_announce failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static void rns_push_status(lua_State* L, const PyxisEvent& ev) {
    lua_getglobal(L, "require");
    lua_pushstring(L, "lib/rns");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        SLog.printf("[rns] require failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    lua_getfield(L, -1, "__dispatch_status");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    lua_pushstring(L, ev.text);        // arg1: text
    lua_pushinteger(L, ev.ts);         // arg2: ts
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        SLog.printf("[rns] __dispatch_status failed: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

void rns_bridge_dispatch(lua_State* L, const PyxisEvent& ev) {
    if (!L) return;   // VM torn down (ELF run in progress) — caller should
                       // already guard this, but never assume.
    switch (ev.kind) {
        case PyxisEvent::MSG_RECEIVED:  rns_push_msg(L, ev);       break;
        case PyxisEvent::MSG_DELIVERED: rns_push_delivered(L, ev); break;
        case PyxisEvent::ANNOUNCE:      rns_push_announce(L, ev);  break;
        case PyxisEvent::RNS_STATUS:    rns_push_status(L, ev);    break;
        default: break;   // MISSED_CALL etc. — out of scope (no calls yet)
    }
}
