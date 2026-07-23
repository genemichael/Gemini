#pragma once

// Control-plane Lua binding for the Pyxis RNS/LXMF service (Lua -> C++ and
// C++ -> Lua). See rns_bridge.cpp. Mirrors fs_bridge.h's shape; the event
// dispatch half mirrors punkmesh.cpp's lua_mesh_push_* family.
//
// Scope: control-plane only (identity/dest/send/announce + status events).
// No audio, no call FSM — that lands with the LXST work (HYBRID_PLAN M3+).

#include "PyxisService.h"   // PyxisEvent

struct lua_State;

// Registers the _rns_* globals on the given Lua state. Call once from
// setupLuaVGL(), next to fs_bridge_register(L) — it re-runs on every Lua
// VM rebuild (native ELF teardown/bring-up), same as every other bridge.
void rns_bridge_register(lua_State* L);

// Pushes one PyxisEvent into lib/rns.lua's __dispatch_* functions. Called
// from drain_rns_events() (main.cpp) for each event drained off
// pyxis_event_queue(). Never lets a Lua error propagate — every step is
// guarded and every lua_pcall'd; safe to call speculatively.
void rns_bridge_dispatch(lua_State* L, const PyxisEvent& ev);
