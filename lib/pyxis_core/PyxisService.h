// SPDX-License-Identifier: GPL-3.0-or-later
//
// PyxisService — headless Reticulum/LXMF (and later LXST) service for the
// T-Deck hybrid phone. Reimplements the non-UI service logic of pyxis
// (https://github.com/torlando-tech/pyxis, GPL-3.0) with the LVGL UI, the
// SX1262 radio interface, and the BLE interface removed; per-function
// provenance is cited in PyxisService.cpp against ../pyxis/src/main.cpp.
//
// Design record: docs/hybrid/HYBRID_PLAN.md (D1-D3, D6-D8, D11).
// The SX1262 belongs exclusively to MeshCore; this service's interfaces
// are AutoInterface (primary, local WiFi) and an optional NVS-gated
// TCPClientInterface (off by default). The device is a fully standalone
// RNS node — no rnsd or remote-daemon dependency, ever.
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Events posted by the service task (core 1) and drained on core 0 by
// drain_rns_events() in main.cpp, mirroring rx_event_queue / RxEvent
// (src/meshpunk_sync.h). Bodies are truncated for the queue; the full
// message is already persisted in the LXMF MessageStore by the time the
// event is posted.
struct PyxisEvent {
    enum Kind : uint8_t {
        MSG_RECEIVED,    // peer_hash = source dest hash, text = content
        MSG_DELIVERED,   // peer_hash = message hash (delivery confirm)
        ANNOUNCE,        // peer_hash = dest hash, peer_name = display name
        RNS_STATUS,      // text = "online"/"offline" summary
        MISSED_CALL,     // peer_hash = caller identity hash (ring timeout)
        CALL_INCOMING,   // peer_hash = caller identity hash — ring the UI
        CALL_STATE       // text = state name ("RINGING", "ACTIVE", ...)
    } kind;
    char peer_hash[33];  // 16-byte hash, hex, NUL-terminated
    char peer_name[32];  // display name when known (announces), else ""
    char text[192];      // truncated content / status detail
    uint8_t aspect;      // ANNOUNCE only: 0 = lxmf.delivery, 1 = lxst.telephony
    uint32_t ts;         // seconds since epoch (0 if wall clock not synced)
};

// ---- lifecycle (called from main.cpp) ----
// Spawns the pyxis_svc task (core 1, prio 2). Returns false if disabled
// via NVS (namespace "pyxis", key "rns_en") or already running. Safe to
// call once from setup() after the mesh task is up.
bool pyxis_service_start();
bool pyxis_service_running();

// Event queue for drain_rns_events(); NULL until pyxis_service_start().
QueueHandle_t pyxis_event_queue();

// D6: true while the service wants WiFi held up — main.cpp's
// wifi_radio_park() must return early when this is set.
bool pyxis_wifi_hold();

// ---- host hooks (implemented in main.cpp; keep the service decoupled
// from launcher internals) ----
// Ask the launcher to (re)start a WiFi connection round (wifi_auto_kick).
void pyxis_host_wifi_kick();
#ifdef HYBRID_TEST_HOOKS
// Complete non-T: serial line, forwarded to the MeshCore CLI under
// MESH_LOCK (HYBRID_PLAN D11).
void pyxis_host_serial_line(const char* line);
// Fed by drain_rns_events() so T:RX reflects what actually reached the
// UI-facing queue (not just the router callback).
void pyxis_test_record_rx_event(const PyxisEvent& ev);
#endif

// ---- thread-safe control API (Lua bindings / test hooks) ----
// All return false/-1 on failure; hex hashes are 32 chars + NUL.
bool pyxis_get_identity_hash(char out_hex[33]);
bool pyxis_get_delivery_dest(char out_hex[33]);
// Queue an outbound LXMF text message (DIRECT; falls back to an
// unresolved-destination send exactly like pyxis's T:SEND when the
// peer's identity isn't cached yet). out_msg_hash may be NULL.
bool pyxis_send_lxmf(const char* dest_hex, const char* text,
                     char out_msg_hash[33]);
bool pyxis_announce();   // announce the LXMF delivery destination now
// Display name carried in LXMF announces. Set persists to NVS
// (pyxis/disp_name), applies to the router live, and re-announces.
// Empty name is allowed (peers show the hash). Max 31 chars.
bool pyxis_set_display_name(const char* name);
bool pyxis_get_display_name(char out[32]);
// Optional TCP client interface (D1: off by default, never required —
// AutoInterface is the primary path). Set persists to NVS and, when
// enabling with WiFi up, connects immediately; disabling takes effect
// at next boot (interfaces aren't detached live). host buffer: 64.
bool pyxis_set_tcp(bool enabled, const char* host, uint16_t port);
bool pyxis_get_tcp(bool* enabled, char host_out[64], uint16_t* port,
                   bool* online);
// AutoInterface on/off (owner decision 2026-07-23: optional — congested
// 2.4GHz LANs flap its multicast carrier). Persists to NVS; takes
// effect at next boot (interfaces are not detached live).
bool pyxis_set_auto_en(bool enabled);
bool pyxis_get_auto_en(bool* enabled, bool* running);

// ---- persisted-conversation snapshots ----
// Commissioned by Gene 2026-08-07 (TESTLOG "Live-bench session" UI
// findings); design ported from wadamesh PyxisService.cpp
// (LIST_CONVS/READ_THREAD/MARK_READ), minus its fork gates.
// HARD RULE (field-diagnosed on wadamesh 2026-07-31): RNS container
// types (Bytes, vector<Bytes>, anything touching the TLSF PSRAM pool)
// exist on the SERVICE task ONLY — the pool is not thread-safe, and
// off-task store reads corrupted it. Callers (Lua bindings, UI) get
// these plain-C snapshots, marshalled through the service task; point
// the out-arrays at PSRAM (they are bulky).
struct PyxisConvRow {
    char     peer_hex[33];   // 16-byte peer hash, hex, NUL-terminated
    char     name[48];       // store display name ("" if never seen)
    char     last_text[64];  // last message snippet ("" if unloadable)
    uint32_t last_ts;        // epoch seconds of last activity (0 unknown)
    uint16_t unread;
    uint16_t msg_count;
};
struct PyxisMsgRow {
    char     text[161];      // truncated content
    uint32_t ts;             // epoch seconds (0 if unknown)
    uint8_t  incoming;       // 1 = inbound
    uint8_t  state;          // LXMF::Type::Message::State as int
};
// Fill out[0..max_rows) newest-activity-first. Returns rows written
// (0 on failure/service down). max_rows is clamped to the store's
// conversation cap (32).
int pyxis_list_conversations(PyxisConvRow* out, int max_rows);
// Last max_rows messages of one conversation, OLDEST FIRST, plus the
// peer's display name (name_out may be NULL). Returns rows written,
// -1 on failure/service down. max_rows is clamped to the store's
// per-conversation cap (256); ask for far fewer — every row is a
// metadata file read on the service task.
int pyxis_read_thread(const char* peer_hex, PyxisMsgRow* out, int max_rows,
                      char name_out[48]);
// Clear a conversation's unread count (store write, service task).
bool pyxis_mark_read(const char* peer_hex);
