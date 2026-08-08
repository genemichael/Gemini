// SPDX-License-Identifier: GPL-3.0-or-later
//
// LXST voice-call engine for the T-Deck hybrid phone. Headless
// reimplementation of pyxis's call state machine (which lives inside
// its LVGL UIManager — see FINDINGS.md §4); wire format and state
// transitions ported 1:1 from ../pyxis/lib/tdeck_ui/UI/LXMF/
// UIManager.{h,cpp} ("UIM:<line>" citations in the .cpp). Audio per
// HYBRID_PLAN D5 (mic) + D12 (speaker, superseding D4's mixer-pull
// design): mic capture via lxst_audio's I2SCapture (I2S_NUM_1 +
// ES7210, exclusive to calls); speaker via lxst_audio's I2SPlayback,
// which takes exclusive ownership of I2S_NUM_0 for call duration only
// — sound.cpp's sound_i2s0_acquire_for_call()/sound_i2s0_restore_after_call()
// swap the boot-installed mixer driver out and back in around the call
// (D4's mixer-pull path measurably garbled RX audio; D12 replaced it
// with native 8kHz playback, no resample). Outside a call, or once a
// call ends, I2S0 is byte-identical to stock (HYBRID_PLAN rule 6).
#pragma once

#include <Arduino.h>

// ── service-internal wiring (called by PyxisService only) ──
namespace RNS { class Identity; }

// Create the lxst.telephony IN destination + announce handler. Runs on
// the service task after the LXMF router exists.
bool pyxis_call_setup(RNS::Identity& identity);
// Per-tick pump on the service task: signal queue, timeouts, TX audio.
void pyxis_call_update();
// Announce the telephony destination (piggybacks the announce ladder).
void pyxis_call_announce();

// ── core-0 marshalling (called from meshpunk's loop(); HYBRID_PLAN D5:
// all ES7210 register I/O shares the Wire bus with touch/keyboard, so
// it must execute on core 0's loop context) ──
void pyxis_call_core0_service();

// ── thread-safe control API (Lua bindings / test hooks / CLI) ──
// dest_hex: any destination hash whose identity is cached (the LXMF
// delivery hash from an announce works — the engine recalls the
// identity and derives the peer's lxst.telephony destination itself,
// exactly like pyxis; UIM:940-951).
bool pyxis_call_initiate(const char* dest_hex);
bool pyxis_call_answer();     // valid only in INCOMING_RINGING
bool pyxis_call_hangup();     // valid in any non-IDLE state
bool pyxis_call_set_mute(bool muted);
bool pyxis_call_muted();
// State name string ("IDLE", "RINGING", "ACTIVE", ...) — stable strings,
// safe to hand to Lua.
const char* pyxis_call_state_name();
// Seconds in ACTIVE state (0 when not active).
uint32_t pyxis_call_duration_s();
// Peer of the current/last call as 32-hex identity hash ("" when none).
void pyxis_call_peer(char out_hex[33]);
// Diagnostics for T:CALL_STATS.
void pyxis_call_stats(uint32_t* tx, uint32_t* rx, int* play_buffered,
                      int* cap_avail, uint32_t* underruns);
// Preferred LXST profile (0x10 ULBW/700C, 0x20 VLBW/1600, 0x30 LBW/3200).
bool pyxis_call_set_profile(int profile);
int  pyxis_call_get_profile();
#ifdef HYBRID_TEST_HOOKS
void pyxis_call_set_inject_sine(bool enabled, int freq, float amp);
#endif
