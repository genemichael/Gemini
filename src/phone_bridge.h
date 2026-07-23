#pragma once

// Control-plane Lua binding for the Pyxis LXST voice-call engine (Lua -> C++
// and C++ -> Lua). See phone_bridge.cpp. Mirrors rns_bridge.h's shape exactly
// (itself mirroring fs_bridge.h) — this is the call-FSM sibling of rns_bridge,
// added once PyxisCall.h's headless engine (HYBRID_PLAN M3+) exists.
//
// Scope: call control-plane only (dial/answer/hangup/state/mute + the three
// call-lifecycle events). No audio crosses the Lua VM — that stays entirely
// inside the native call engine (PyxisCall.{h,cpp} + lxst_audio).

#include "PyxisService.h"   // PyxisEvent

struct lua_State;

// Registers the _phone_* globals on the given Lua state. Call once from
// setupLuaVGL(), next to rns_bridge_register(L) — it re-runs on every Lua VM
// rebuild (native ELF teardown/bring-up), same as every other bridge.
void phone_bridge_register(lua_State* L);

// Pushes one PyxisEvent into lib/phone.lua's __dispatch_* functions. Called
// from drain_rns_events() (main.cpp) for each event drained off
// pyxis_event_queue(), alongside rns_bridge_dispatch — this function only
// handles CALL_INCOMING / CALL_STATE / MISSED_CALL and ignores every other
// kind (rns_bridge_dispatch owns those). Never lets a Lua error propagate —
// every step is guarded and every lua_pcall'd; safe to call speculatively.
void phone_bridge_dispatch(lua_State* L, const PyxisEvent& ev);
