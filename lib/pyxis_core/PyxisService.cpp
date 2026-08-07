// SPDX-License-Identifier: GPL-3.0-or-later
//
// PyxisService implementation. Reimplements the non-UI service logic of
// pyxis (https://github.com/torlando-tech/pyxis, GPL-3.0) — provenance
// for each block is cited against ../pyxis/src/main.cpp ("pyxis:<line>")
// as of pyxis main @ 2026-07-22. See docs/hybrid/HYBRID_PLAN.md D1-D3,
// D6-D8, D11 for the design record.
//
// Threading model: everything Reticulum/LXMF runs on ONE task
// (`pyxis_svc`, core 1, prio 2) — mirroring pyxis, where the whole stack
// ran on loopTask. Cross-task control (Lua bindings on core 0, MeshCore
// CLI on core 1) never touches RNS objects directly: commands marshal
// through s_cmd_q and execute between service-loop steps. Identity and
// delivery-destination hashes are cached once before the service is
// marked running, so those reads are lock-free.

#include "PyxisService.h"

#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <atomic>
#include <esp_heap_caps.h>

#include <microReticulum/Reticulum.h>
#include <microReticulum/Utilities/OS.h>
#include <microStore/Adapters/LittleFSFileSystem.h>
#include <microReticulum/Identity.h>
#include <microReticulum/Destination.h>
#include <microReticulum/Transport.h>
#include <microReticulum/Interface.h>
#include <microReticulum/Log.h>
#include <LXMF/LXMRouter.h>
#include <LXMF/MessageStore.h>

#include "TCPClientInterface.h"
#include "AutoInterface.h"
#include "PyxisCall.h"
#include "pyxis_internal.h"

#ifdef HYBRID_TEST_HOOKS
#include <LittleFS.h>
// Private base64 decoder for T:FDATA. Deliberately NOT <base64.hpp>:
// densaugeo/base64 defines its functions non-inline in the header, and
// MeshCore's BaseChatMesh.cpp already claims the one allowed definition
// — a second include here is a duplicate-symbol link error.
static unsigned b64_decode_(const char* in, unsigned in_len, uint8_t* out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;   // '=' padding and anything else stop the decode
    };
    unsigned o = 0;
    int acc = 0, bits = 0;
    for (unsigned i = 0; i < in_len; i++) {
        int v = val(in[i]);
        if (v < 0) break;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    return o;
}
#endif

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// Settings, NVS namespace "pyxis" (renamed from pyxis's generic
// "settings" per HYBRID_PLAN D7 to avoid colliding with launcher NVS).
// Defaults implement D1: AutoInterface on, TCP off with an EMPTY host
// (pyxis defaulted tcp_en=true + a public relay host — both reversed).
struct SvcSettings {
    bool     rns_en = true;
    bool     auto_en = true;
    bool     tcp_en = false;
    String   tcp_host;            // empty = never construct TCP interface
    uint16_t tcp_port = 4965;
    String   display_name;
    uint32_t announce_interval = 3600;  // seconds, pyxis:541
};
static SvcSettings s_cfg;

static RNS::Reticulum*     s_reticulum = nullptr;
static RNS::Identity*      s_identity = nullptr;
static LXMF::LXMRouter*    s_router = nullptr;
static LXMF::MessageStore* s_store = nullptr;

static AutoInterface*      s_auto_impl = nullptr;
static RNS::Interface*     s_auto_if = nullptr;
static TCPClientInterface* s_tcp_impl = nullptr;
static RNS::Interface*     s_tcp_if = nullptr;

static TaskHandle_t  s_task = nullptr;
static QueueHandle_t s_evq = nullptr;
static volatile bool s_running = false;

static char s_identity_hex[33] = {0};
static char s_dest_hex[33] = {0};
// Lock-free read copy of the display name (written on the service task
// before s_running and inside SET_NAME command execution; racing reads
// at worst see a torn but NUL-terminated old/new mix for one call).
static char s_disp_name[32] = {0};

// Lock-free read copy of the TCP config (same discipline as
// s_disp_name: written on the service task, torn reads harmless).
static char s_tcp_host_c[64] = {0};

// Cross-task command marshalling (see file header).
struct SvcCmd {
    enum Op : uint8_t { SEND, ANNOUNCE, SET_NAME, SET_TCP,
                        LIST_CONVS, READ_THREAD, MARK_READ } op;
    char dest_hex[33];
    const char* text;            // copied into the heap block's tail by run_cmd
    char out_hash[33];
    uint16_t port;               // SET_TCP
    bool flag;                   // SET_TCP: enabled
    // Conversation snapshots (LIST_CONVS/READ_THREAD; commissioned by
    // Gene 2026-08-07, TESTLOG "Live-bench session" UI findings; op
    // shape from wadamesh PyxisService.cpp). `dest` is the CALLER'S
    // out-array and is written ONLY by run_cmd on the DONE path: the
    // service fills the heap block's tail (run_cmd sizes it) and never
    // dereferences `dest`. Deliberate hardening over the wadamesh
    // reference, which writes through caller pointers DURING execution
    // — under the abandon handshake below those buffers may be dead by
    // then. Same discipline as the `text` tail, in reverse. Ops with an
    // output tail carry no text (both live at (cmd + 1)).
    void* dest;                  // caller out-array; run_cmd-only
    int   max_rows;              // capacity of the output tail, in rows
    int   rows;                  // result: rows written
    char  name_buf[48];          // READ_THREAD result: peer display name
    bool ok;
    SemaphoreHandle_t done;
    // Ownership handshake — backport from wadamesh PyxisService.cpp per
    // docs/hybrid/WADAMESH_BACKPORT_BRIEF.md §1 (field-proven
    // 2026-07-31): run_cmd's old stack-resident command died when its
    // 2s wait timed out while the service was FS-stalled — the service
    // then executed the stale pointer and Gave a dead semaphore
    // (xQueueGenericSend assert queue.c:821). Commands are now
    // HEAP-owned with an atomic state; exactly one side frees:
    //   PENDING -> DONE      (service won: it Gives, caller frees)
    //   PENDING -> ABANDONED (caller timed out: service frees, no Give)
    // Deliberate hardening over the wadamesh reference (which still has
    // a dangling-text window): run_cmd copies `text` into the block's
    // tail so an abandoning caller can't free it out from under do_send.
    enum : uint8_t { CMD_PENDING = 0, CMD_DONE = 1, CMD_ABANDONED = 2 };
    std::atomic<uint8_t> state{CMD_PENDING};
    StaticSemaphore_t sem_buf;
};
static QueueHandle_t s_cmd_q = nullptr;

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

static void post_event(PyxisEvent::Kind kind, const char* hash_hex,
                       const char* name, const char* text,
                       uint8_t aspect = 0) {
    if (!s_evq) return;
    PyxisEvent ev = {};
    ev.kind = kind;
    if (hash_hex) strlcpy(ev.peer_hash, hash_hex, sizeof(ev.peer_hash));
    if (name)     strlcpy(ev.peer_name, name, sizeof(ev.peer_name));
    if (text)     strlcpy(ev.text, text, sizeof(ev.text));
    ev.aspect = aspect;
    ev.ts = (uint32_t)time(nullptr);
    // Queue full → drop announces first, never a message event
    // (HYBRID_PLAN D2). Message events overwrite the oldest ANNOUNCE
    // slot is not possible with a plain queue, so: announces are
    // best-effort sends, message events retry once after evicting one
    // stale event.
    if (xQueueSend(s_evq, &ev, 0) != pdTRUE) {
        if (kind == PyxisEvent::ANNOUNCE) return;
        PyxisEvent scratch;
        xQueueReceive(s_evq, &scratch, 0);
        xQueueSend(s_evq, &ev, 0);
    }
}

// ---------------------------------------------------------------------------
// Settings + identity (pyxis:482-559, 870-902)
// ---------------------------------------------------------------------------

static void load_settings() {
    Preferences prefs;
    prefs.begin("pyxis", true);
    s_cfg.rns_en            = prefs.getBool("rns_en", true);
    s_cfg.auto_en           = prefs.getBool("auto_en", true);
    s_cfg.tcp_en            = prefs.getBool("tcp_en", false);
    s_cfg.tcp_host          = prefs.getString("tcp_host", "");
    s_cfg.tcp_port          = prefs.getUShort("tcp_port", 4965);
    s_cfg.display_name      = prefs.getString("disp_name", "");
    s_cfg.announce_interval = prefs.getULong("announce", 3600);
    prefs.end();
    strlcpy(s_tcp_host_c, s_cfg.tcp_host.c_str(), sizeof(s_tcp_host_c));

    // Migration: a device previously running stock pyxis has its display
    // name in pyxis's old NVS namespace ("settings"/"disp_name",
    // pyxis:518). Fall back to it once so the announce name survives the
    // firmware swap; NEVER read that namespace's interface settings
    // (tcp_en etc.) — the hybrid's D1 defaults must win.
    if (s_cfg.display_name.isEmpty()) {
        Preferences legacy;
        if (legacy.begin("settings", true)) {
            s_cfg.display_name = legacy.getString("disp_name", "");
            legacy.end();
            if (!s_cfg.display_name.isEmpty()) {
                Preferences save;
                save.begin("pyxis", false);
                save.putString("disp_name", s_cfg.display_name);
                save.end();
            }
        }
    }
}

// Load-or-create the RNS identity. NVS survives reflashes (unlike the
// data partition), so the device keeps its RNS identity across firmware
// updates. Ported from pyxis:872-902; namespace kept as "reticulum"
// (HYBRID_PLAN D7 — MeshCore never uses this namespace).
static void load_or_create_identity() {
    Preferences prefs;
    prefs.begin("reticulum", false);
    size_t key_len = prefs.getBytesLength("identity");
    if (key_len == 64) {
        uint8_t key_data[64];
        prefs.getBytes("identity", key_data, 64);
        RNS::Bytes private_key(key_data, 64);
        s_identity = new RNS::Identity(false);
        if (!s_identity->load_private_key(private_key)) {
            Serial.println("[pyxis] identity load failed, creating new");
            s_identity = new RNS::Identity();
            RNS::Bytes priv = s_identity->get_private_key();
            prefs.putBytes("identity", priv.data(), priv.size());
        }
    } else {
        Serial.println("[pyxis] no identity in NVS, creating");
        s_identity = new RNS::Identity();
        RNS::Bytes priv = s_identity->get_private_key();
        prefs.putBytes("identity", priv.data(), priv.size());
    }
    prefs.end();
}

// ---------------------------------------------------------------------------
// Interfaces (pyxis:1354-1404). AutoInterface is primary (D1); both
// helpers are idempotent and re-invoked from the WiFi watcher.
// ---------------------------------------------------------------------------

static void start_auto_interface() {
    if (!s_cfg.auto_en || WiFi.status() != WL_CONNECTED) return;
    if (!s_auto_impl) {
        Serial.println("[pyxis] starting AutoInterface (IPv6 peer discovery)");
        s_auto_impl = new AutoInterface("Auto");
        s_auto_if = new RNS::Interface(s_auto_impl);
        if (s_auto_if->start()) {
            RNS::Transport::register_interface(*s_auto_if);
        } else {
            Serial.println("[pyxis] AutoInterface start FAILED");
        }
    } else if (!s_auto_if->online()) {
        // Exists but stopped (post-disconnect): restart. pyxis:1368-1375
        if (!s_auto_if->start())
            Serial.println("[pyxis] AutoInterface restart failed");
    }
}

static void start_tcp_interface() {
    // D1: optional, off by default, never constructed without a host.
    if (!s_cfg.tcp_en || s_cfg.tcp_host.isEmpty()) return;
    if (WiFi.status() != WL_CONNECTED) return;
    if (!s_tcp_impl) {
        s_tcp_impl = new TCPClientInterface("tcp0");
        s_tcp_impl->set_target_host(s_cfg.tcp_host.c_str());
        s_tcp_impl->set_target_port(s_cfg.tcp_port);
        s_tcp_if = new RNS::Interface(s_tcp_impl);
        if (!s_tcp_if->start()) {
            // Self-reconnects via its background task; register anyway.
            Serial.println("[pyxis] TCP initial connect failed, will retry");
        }
        RNS::Transport::register_interface(*s_tcp_if);   // pyxis:1393-1396
    } else {
        // Resave with a live interface: force the socket down first so
        // the background task reconnects against the (possibly new)
        // target — start() alone no-ops when the task already runs.
        s_tcp_impl->set_target_host(s_cfg.tcp_host.c_str());
        s_tcp_impl->set_target_port(s_cfg.tcp_port);
        s_tcp_impl->disconnect();
        s_tcp_impl->start();
        Serial.println("[pyxis] TCP retarget: forced reconnect");
    }
}

// ---------------------------------------------------------------------------
// LXMF announce handler → ANNOUNCE events (shape from pyxis
// UIManager.cpp:36-42, LXSTAnnounceHandler)
// ---------------------------------------------------------------------------

class LxmfAnnounceHandler : public RNS::AnnounceHandler {
public:
    LxmfAnnounceHandler() : RNS::AnnounceHandler("lxmf.delivery") {}
    void received_announce(const RNS::Bytes& dest_hash,
                           const RNS::Identity& identity,
                           const RNS::Bytes& app_data) override {
        // LXMF app_data begins with the peer's display name (the LXMF
        // announce format); take the printable prefix, defensively.
        char name[32] = {0};
        size_t n = 0;
        const uint8_t* p = app_data.data();
        for (size_t i = 0; p && i < app_data.size() && n < sizeof(name) - 1; ++i) {
            if (p[i] < 0x20 || p[i] > 0x7e) break;
            name[n++] = (char)p[i];
        }
        post_event(PyxisEvent::ANNOUNCE, dest_hash.toHex().c_str(), name, nullptr);
    }
};
static std::shared_ptr<LxmfAnnounceHandler> s_announce_handler;

// ---------------------------------------------------------------------------
// NTP (pyxis:617-772). LXMF needs wall clock; meshpunk has none (its
// time comes from GPS/RTC for its own use). Kick on WiFi connect, poll
// non-blocking from the service loop, feed OS::setTimeOffset once.
// ---------------------------------------------------------------------------

static bool     s_ntp_pending = false;
static uint32_t s_ntp_start_ms = 0;
static const uint32_t NTP_TIMEOUT_MS = 10000;

static void ntp_kick() {
    configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");
    s_ntp_pending = true;
    s_ntp_start_ms = millis();
}

static void ntp_pump() {
    if (!s_ntp_pending) return;
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        // 64-bit rolling uptime, not raw millis() — pyxis:749-752.
        time_t now = time(nullptr);
        uint64_t uptime_ms = RNS::Utilities::OS::ltime() -
                             RNS::Utilities::OS::getTimeOffset();
        RNS::Utilities::OS::setTimeOffset((uint64_t)now * 1000 - uptime_ms);
        Serial.println("[pyxis] NTP time synced");
        s_ntp_pending = false;
        return;
    }
    if (millis() - s_ntp_start_ms >= NTP_TIMEOUT_MS) {
        Serial.println("[pyxis] NTP sync timed out");
        s_ntp_pending = false;
    }
}

// ---------------------------------------------------------------------------
// Reticulum + LXMF bring-up (pyxis:847-994, 996-1078, 1596-1633)
// ---------------------------------------------------------------------------

static bool service_init() {
    // Filesystem: microStore's LittleFS adapter reuses the partition
    // labeled "spiffs" (= meshpunk's 6MB data partition, already
    // LittleFS). LittleFS.begin() is idempotent, so this coexists with
    // the launcher's earlier mount. pyxis:790-800.
    static microStore::Adapters::LittleFSFileSystem fs;
    if (!fs.init()) {
        Serial.println("[pyxis] FATAL: filesystem mount failed");
        return false;
    }
    RNS::Utilities::OS::register_filesystem(fs);

    s_reticulum = new RNS::Reticulum();
    // Required: without transport mode, Transport::start() skips path-
    // store init and every announce fails to enter the path table.
    // pyxis:853-864.
    RNS::Reticulum::transport_enabled(true);
    // LOG_INFO (was WARNING): with AutoInterface off by default the RNS
    // log volume is small, and WARNING-only made every TCP connect
    // attempt/failure INVISIBLE — the interface sat offline for days
    // with zero log evidence (diagnosed 2026-07-23). INFO is what shows
    // "Connected to <host>" / "Initial connection failed".
    RNS::loglevel(RNS::LOG_INFO);

    load_or_create_identity();
    strlcpy(s_identity_hex, s_identity->hash().toHex().c_str(),
            sizeof(s_identity_hex));

    // Interfaces before reticulum->start(), matching pyxis's ordering
    // (pyxis:908-993). Both helpers no-op if WiFi isn't up yet; the
    // WiFi watcher in the service loop retries them.
    start_auto_interface();
    start_tcp_interface();
    s_reticulum->start();

    // LXMF. Store and router share "/lxmf" (pyxis:1000,1021). No SD
    // archive tier in M2 (launcher owns the SD card; revisit if soak
    // tests fill the 6MB partition — pyxis:1003-1018).
    s_store = new LXMF::MessageStore("/lxmf");
    s_router = new LXMF::LXMRouter(*s_identity, "/lxmf");
    strlcpy(s_disp_name, s_cfg.display_name.c_str(), sizeof(s_disp_name));
    if (!s_cfg.display_name.isEmpty())
        s_router->set_display_name(s_cfg.display_name.c_str());

    strlcpy(s_dest_hex, s_router->delivery_destination().hash().toHex().c_str(),
            sizeof(s_dest_hex));

    // Inbound: first pyxis-owned touch of a decoded message — persist,
    // then post the event (pyxis UIManager.cpp:290-292 + 801-848 with
    // the LVGL parts removed).
    s_router->register_delivery_callback([](LXMF::LXMessage& msg) {
        if (s_store) s_store->save_message(msg);
        std::string content((const char*)msg.content().data(),
                            msg.content().size());
        post_event(PyxisEvent::MSG_RECEIVED,
                   msg.source_hash().toHex().c_str(), nullptr,
                   content.c_str());
    });

    // Outbound delivery confirmations (pyxis:1596-1633, sans UI).
    s_router->register_delivered_callback([](LXMF::LXMessage& msg) {
        RNS::Bytes h = msg.hash();
        if (s_store)
            s_store->update_message_state(h, LXMF::Type::Message::DELIVERED);
        post_event(PyxisEvent::MSG_DELIVERED, h.toHex().c_str(),
                   nullptr, nullptr);
    });

    s_announce_handler = std::make_shared<LxmfAnnounceHandler>();
    RNS::Transport::register_announce_handler(
        RNS::HAnnounceHandler(s_announce_handler));

    // M3: LXST voice — telephony IN destination + announce handler.
    pyxis_call_setup(*s_identity);

    return true;
}

// pyxis_internal.h seams for PyxisCall.cpp.
void pyxis_internal_post_event(PyxisEvent::Kind kind, const char* hash_hex,
                               const char* name, const char* text,
                               uint8_t aspect) {
    post_event(kind, hash_hex, name, text, aspect);
}
LXMF::LXMRouter* pyxis_internal_router() { return s_router; }

// ---------------------------------------------------------------------------
// Outbound send (pyxis:1856-1886, the T:SEND implementation — identical
// semantics: DIRECT method, destination resolved from the identity
// cache when known, raw destination hash otherwise)
// ---------------------------------------------------------------------------

static bool parse_hex16(const char* hex, RNS::Bytes& out) {
    if (!hex) return false;
    size_t len = strlen(hex);
    if (len != 32) return false;
    for (size_t i = 0; i + 1 < len; i += 2) {
        char buf[3] = {hex[i], hex[i + 1], 0};
        char* end = nullptr;
        long v = strtol(buf, &end, 16);
        if (end != buf + 2) return false;
        out << (uint8_t)v;
    }
    return out.size() == 16;
}

static bool do_send(const char* dest_hex, const char* text,
                    char out_hash[33]) {
    if (!s_router || !text) return false;
    RNS::Bytes dest_hash;
    if (!parse_hex16(dest_hex, dest_hash)) return false;

    RNS::Identity dest_identity = RNS::Identity::recall(dest_hash);
    RNS::Destination destination(RNS::Type::NONE);
    if (dest_identity) {
        destination = RNS::Destination(dest_identity,
                                       RNS::Type::Destination::OUT,
                                       RNS::Type::Destination::SINGLE,
                                       "lxmf", "delivery");
    }
    RNS::Bytes content((const uint8_t*)text, strlen(text));
    RNS::Bytes title;
    LXMF::LXMessage msg(destination, s_router->delivery_destination(),
                        content, title, LXMF::Type::Message::DIRECT);
    if (!dest_identity) msg.destination_hash(dest_hash);
    msg.pack();
    if (s_store) s_store->save_message(msg);
    s_router->handle_outbound(msg);
    if (out_hash)
        strlcpy(out_hash, msg.hash().toHex().c_str(), 33);
    return true;
}

// ---------------------------------------------------------------------------
// Test hooks (HYBRID_PLAN D11) — service task is the sole Serial line
// reader in HYBRID_TEST_HOOKS builds. Protocol ported from pyxis's
// PYXIS_TEST_HOOKS block (pyxis:1690-2266): one line in, one T:OK/T:ERR
// terminal reply (multi-line dumps prefixed by a `count=` header).
// ---------------------------------------------------------------------------

// Defined with the service-task command executor below; the T:NAME test
// hook needs it earlier in the file.
static bool do_set_display_name(const char* name);

#ifdef HYBRID_TEST_HOOKS
static size_t s_rx_total = 0;
static File   s_put_file;      // T:FOPEN/T:FDATA/T:FCLOSE transfer state
static size_t s_put_total = 0;
static long   s_put_seq = -1;  // last applied chunk seq (idempotent resend)
struct TestRx { char src[33]; char content[128]; };
static TestRx s_rx_ring[16];
static size_t s_rx_count = 0;

static void test_hook_record_rx(const PyxisEvent& ev) {
    s_rx_total++;
    if (s_rx_count >= 16) return;
    strlcpy(s_rx_ring[s_rx_count].src, ev.peer_hash, 33);
    strlcpy(s_rx_ring[s_rx_count].content, ev.text, 128);
    s_rx_count++;
}

static void handle_test_command(const String& line) {
    int sep = line.indexOf(' ');
    String cmd = (sep < 0) ? line : line.substring(0, sep);
    String args = (sep < 0) ? "" : line.substring(sep + 1);

    if (cmd == "T:ID") {
        Serial.println(String("T:OK ") + s_identity_hex);
    } else if (cmd == "T:DEST") {
        Serial.println(String("T:OK ") + s_dest_hex);
    } else if (cmd == "T:ANN") {
        if (!s_router) { Serial.println("T:ERR no router"); return; }
        s_router->announce();
        Serial.println("T:OK announced");
    } else if (cmd == "T:STATE") {
        // Interface + WiFi one-liner for harness wait loops.
        // One println: other tasks' serial output interleaves between
        // separate print() calls and shredded this reply on the wire.
        String r = "T:OK wifi=";
        r += (WiFi.status() == WL_CONNECTED) ? "1" : "0";
        r += " auto=";
        r += (s_auto_if && s_auto_if->online()) ? "1" : "0";
        r += " tcp=";
        r += (s_tcp_if && s_tcp_if->online()) ? "1" : "0";
        r += " time=" + String((uint32_t)time(nullptr));
        // svc stack high-water + heap floor: the evidence base for
        // stack trims (never trim blind — the 16KB capture-stack
        // experiment ended in a Guru Meditation).
        r += " svc_hwm=" + String((unsigned)uxTaskGetStackHighWaterMark(nullptr));
        r += " int_free=" + String((unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        r += " largest=" + String((unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        Serial.println(r);
    } else if (cmd == "T:PATHS") {
        const auto& pt = RNS::Transport::path_table();
        Serial.print("T:OK count=");
        Serial.println(String((unsigned)pt.size()));
        for (const auto& kv : pt) {
            Serial.print("T:PATH ");
            Serial.println(kv.first.toHex().c_str());
        }
    } else if (cmd == "T:HASPATH") {
        RNS::Bytes dest;
        if (!parse_hex16(args.c_str(), dest)) { Serial.println("T:ERR bad hex"); return; }
        Serial.println(String("T:OK ") +
                       (RNS::Transport::has_path(dest) ? "1" : "0"));
    } else if (cmd == "T:RECALL") {
        RNS::Bytes dest;
        if (!parse_hex16(args.c_str(), dest)) { Serial.println("T:ERR bad hex"); return; }
        RNS::Bytes app = RNS::Identity::recall_app_data(dest);
        Serial.println(String("T:OK size=") + String((unsigned)app.size()) +
                       " hex=" + app.toHex().c_str());
    } else if (cmd == "T:SEND") {
        int sp = args.indexOf(' ');
        if (sp < 0) { Serial.println("T:ERR usage T:SEND <hex> <text>"); return; }
        char hash[33] = {0};
        if (do_send(args.substring(0, sp).c_str(),
                    args.substring(sp + 1).c_str(), hash)) {
            Serial.println(String("T:OK hash=") + hash);
        } else {
            Serial.println("T:ERR send failed");
        }
    } else if (cmd == "T:RX") {
        Serial.print("T:OK count=");
        Serial.println(String((unsigned)s_rx_total));
        for (size_t i = 0; i < s_rx_count; ++i) {
            Serial.print("T:RXMSG src=");
            Serial.print(s_rx_ring[i].src);
            Serial.print(" content=");
            Serial.println(s_rx_ring[i].content);
        }
    } else if (cmd == "T:RXCLR") {
        s_rx_count = 0; s_rx_total = 0;
        Serial.println("T:OK cleared");
    } else if (cmd == "T:FOPEN") {
        // No-wipe Lua-file updates over serial (uploadfs replaces the
        // whole data partition, nuking the MeshCore identity + WiFi
        // creds). Protocol: T:FOPEN <path> → T:FDATA <base64>... →
        // T:FCLOSE. Parent directories are created as needed.
        if (s_put_file) { s_put_file.close(); }
        if (args.length() < 2 || args[0] != '/') {
            Serial.println("T:ERR path must start with /");
        } else {
            for (int i = 1; i < (int)args.length(); i++) {
                if (args[i] == '/') LittleFS.mkdir(args.substring(0, i));
            }
            s_put_file = LittleFS.open(args, "w");
            if (s_put_file) {
                s_put_total = 0;
                s_put_seq = -1;
                Serial.println(String("T:OK open ") + args);
            } else {
                Serial.println("T:ERR open failed");
            }
        }
    } else if (cmd == "T:FDATA") {
        // T:FDATA <seq> <base64> — seq makes lost-reply resends
        // idempotent: a chunk whose seq we already applied is ACKed
        // again without writing (serial replies can vanish into boot/
        // log noise; the push tool retries on timeout).
        if (!s_put_file) {
            Serial.println("T:ERR no open file");
        } else {
            int sp = args.indexOf(' ');
            if (sp < 1) { Serial.println("T:ERR usage T:FDATA <seq> <b64>"); return; }
            long seq = args.substring(0, sp).toInt();
            if (seq == s_put_seq) {
                Serial.println(String("T:OK dup seq=") + String(seq));
                return;
            }
            if (seq != s_put_seq + 1) {
                Serial.println(String("T:ERR seq gap want=") + String(s_put_seq + 1));
                return;
            }
            const char* b64 = args.c_str() + sp + 1;
            unsigned b64_len = args.length() - sp - 1;
            static uint8_t decoded[512];
            if (b64_len > 680) {   // 680 b64 chars ≈ 510 bytes
                Serial.println("T:ERR chunk too large");
                return;
            }
            unsigned int n = b64_decode_(b64, b64_len, decoded);
            if (s_put_file.write(decoded, n) != n) {
                Serial.println("T:ERR write failed");
            } else {
                s_put_total += n;
                s_put_seq = seq;
                Serial.println(String("T:OK n=") + String((unsigned)n) +
                               " seq=" + String(seq));
            }
        }
    } else if (cmd == "T:FCLOSE") {
        if (!s_put_file) {
            Serial.println("T:ERR no open file");
        } else {
            s_put_file.close();
            Serial.println(String("T:OK size=") + String((unsigned)s_put_total));
        }
    } else if (cmd == "T:CALL") {
        if (pyxis_call_initiate(args.c_str())) Serial.println("T:OK calling");
        else Serial.println("T:ERR bad dest or busy");
    } else if (cmd == "T:CALL_STATE") {
        Serial.println(String("T:OK ") + pyxis_call_state_name() +
                       " dur=" + String(pyxis_call_duration_s()));
    } else if (cmd == "T:CALL_ANSWER") {
        Serial.println(pyxis_call_answer() ? "T:OK answering"
                                           : "T:ERR not ringing");
    } else if (cmd == "T:CALL_HANGUP") {
        Serial.println(pyxis_call_hangup() ? "T:OK hanging up"
                                           : "T:ERR no call");
    } else if (cmd == "T:CALL_STATS") {
        uint32_t tx, rx, ur;
        int pb, ca;
        pyxis_call_stats(&tx, &rx, &pb, &ca, &ur);
        Serial.printf("T:OK tx=%lu rx=%lu playbuf=%d capavail=%d underrun=%lu\n",
                      (unsigned long)tx, (unsigned long)rx, pb, ca,
                      (unsigned long)ur);
    } else if (cmd == "T:CALL_PROFILE") {
        if (args.length()) {
            int p = (int)strtol(args.c_str(), nullptr, 0);
            if (!pyxis_call_set_profile(p)) { Serial.println("T:ERR bad profile"); return; }
        }
        Serial.printf("T:OK profile=0x%02X\n", pyxis_call_get_profile());
    } else if (cmd == "T:CALL_INJECT") {
        int on = args.toInt();
        pyxis_call_set_inject_sine(on != 0, 730, 0.5f);
        Serial.println(String("T:OK inject=") + (on ? "1" : "0"));
    } else if (cmd == "T:ANNLXST") {
        pyxis_call_announce();
        Serial.println("T:OK announced");
    } else if (cmd == "T:TCP") {
        // Optional reach-beyond-LAN TCP client (HYBRID_PLAN D1: off by
        // default, never required). Runs on the svc task, so applying
        // directly is safe.
        //   T:TCP                → show current config + online state
        //   T:TCP <host> <port>  → enable + persist + connect now
        //   T:TCP off            → disable (takes effect next boot)
        if (args.isEmpty()) {
            Serial.printf("T:OK en=%d host=%s port=%u online=%d\n",
                          s_cfg.tcp_en ? 1 : 0, s_cfg.tcp_host.c_str(),
                          s_cfg.tcp_port,
                          (s_tcp_if && s_tcp_if->online()) ? 1 : 0);
        } else if (args == "off") {
            s_cfg.tcp_en = false;
            Preferences p;
            p.begin("pyxis", false);
            p.putBool("tcp_en", false);
            p.end();
            Serial.println("T:OK disabled (existing connection drops at reboot)");
        } else {
            int sp = args.indexOf(' ');
            String host = (sp < 0) ? args : args.substring(0, sp);
            uint16_t port = (sp < 0) ? 4965 : (uint16_t)args.substring(sp + 1).toInt();
            if (host.isEmpty() || port == 0) { Serial.println("T:ERR usage T:TCP <host> <port>"); return; }
            s_cfg.tcp_en = true;
            s_cfg.tcp_host = host;
            s_cfg.tcp_port = port;
            Preferences p;
            p.begin("pyxis", false);
            p.putBool("tcp_en", true);
            p.putString("tcp_host", host);
            p.putUShort("tcp_port", port);
            p.end();
            start_tcp_interface();
            Serial.printf("T:OK %s:%u %s\n", host.c_str(), port,
                          (WiFi.status() == WL_CONNECTED)
                              ? "connecting" : "saved (connects when WiFi up)");
        }
    } else if (cmd == "T:AUTO") {
        // Owner decision 2026-07-23: AutoInterface optional (congested
        // 2.4GHz LANs flap its multicast carrier). T:AUTO on|off
        // persists; takes effect at next boot (no live interface
        // detach). With auto off, TCP is the only path.
        if (args == "on" || args == "off") {
            s_cfg.auto_en = (args == "on");
            Preferences p;
            p.begin("pyxis", false);
            p.putBool("auto_en", s_cfg.auto_en);
            p.end();
            Serial.printf("T:OK auto_en=%d - REBOOT to apply\n", s_cfg.auto_en ? 1 : 0);
        } else {
            Serial.printf("T:OK auto_en=%d running=%d\n", s_cfg.auto_en ? 1 : 0,
                          (s_auto_if && s_auto_if->online()) ? 1 : 0);
        }
    } else if (cmd == "T:IDEXPORT") {
        // RNS identity migration (device→device). The 64-byte private
        // key as 128 hex chars — this IS the user's whole RNS persona;
        // the tool prints a warning, the user guards the output.
        Preferences p;
        p.begin("reticulum", true);
        uint8_t key[64];
        size_t n = (p.getBytesLength("identity") == 64)
                       ? p.getBytes("identity", key, 64) : 0;
        p.end();
        if (n != 64) { Serial.println("T:ERR no identity in NVS"); return; }
        char hex[129];
        for (int i = 0; i < 64; i++) sprintf(hex + i * 2, "%02x", key[i]);
        Serial.println(String("T:OK PRIVATE KEY - GUARD IT: ") + hex);
    } else if (cmd == "T:IDNEW") {
        // Generate a brand-new RNS identity (same call the first-boot
        // path uses) and persist it. Applies at next reboot. The OLD
        // identity is gone unless T:IDEXPORT was run first — the
        // command echoes the new private key so it can be backed up.
        RNS::Identity fresh;   // default ctor generates a keypair
        RNS::Bytes priv = fresh.get_private_key();
        if (priv.size() != 64) { Serial.println("T:ERR keygen failed"); return; }
        Preferences p;
        p.begin("reticulum", false);
        p.putBytes("identity", priv.data(), 64);
        p.end();
        char hex[129];
        for (int i = 0; i < 64; i++) sprintf(hex + i * 2, "%02x", priv.data()[i]);
        Serial.println(String("T:OK new identity saved - REBOOT to apply. ") +
                       "Backup (PRIVATE): " + hex);
    } else if (cmd == "T:IDIMPORT") {
        // T:IDIMPORT <128 hex chars> — overwrites this device's RNS
        // identity. Applies at next boot (the running stack keeps the
        // old identity until then; live re-init is not supported).
        if (args.length() != 128) {
            Serial.println("T:ERR need 128 hex chars (64-byte private key)");
            return;
        }
        uint8_t key[64];
        for (int i = 0; i < 64; i++) {
            char b[3] = {args[i * 2], args[i * 2 + 1], 0};
            char* end = nullptr;
            long v = strtol(b, &end, 16);
            if (end != b + 2) { Serial.println("T:ERR bad hex"); return; }
            key[i] = (uint8_t)v;
        }
        Preferences p;
        p.begin("reticulum", false);
        p.putBytes("identity", key, 64);
        p.end();
        Serial.println("T:OK identity imported - REBOOT to apply");
    } else if (cmd == "T:NAME") {
        // Runs on the service task, so apply directly (no marshalling).
        if (do_set_display_name(args.c_str())) {
            Serial.println(String("T:OK name=") + s_disp_name);
        } else {
            Serial.println("T:ERR name failed");
        }
    } else if (cmd == "T:CONVS") {
        // Read-only persisted-conversation dump (commissioned by Gene
        // 2026-08-07 — reboot-survival check for the message store).
        // Runs on the svc task, so direct store reads are legal here;
        // same count= header + per-row line protocol as T:PATHS/T:RX.
        if (!s_store) { Serial.println("T:ERR no store"); return; }
        auto convs = s_store->get_conversations();
        Serial.print("T:OK count=");
        Serial.println(String((unsigned)convs.size()));
        for (auto& p : convs) {
            auto ci = s_store->get_conversation_info(p);
            char snip[64] = {0};
            if (ci.message_count > 0) {
                auto md = s_store->load_message_metadata(
                    ci.last_message_hash_bytes());
                if (md.valid) strlcpy(snip, md.content.c_str(), sizeof(snip));
            }
            // Keep the line protocol intact: message content may hold
            // newlines/controls — flatten them.
            for (char* c = snip; *c; ++c)
                if (*c < 0x20 || *c > 0x7e) *c = '.';
            String r = "T:CONV ";
            r += p.toHex().c_str();
            r += " n=" + String((unsigned)ci.message_count);
            r += " unread=" + String((unsigned)ci.unread_count);
            r += " name="; r += ci.display_name;
            r += " last="; r += snip;
            Serial.println(r);
        }
    } else if (cmd == "VERSION") {
        Serial.println("T:OK hybrid-phone M2");
    } else {
        Serial.println("T:ERR unknown");
    }
}

// Line assembler over Serial; T: lines handled here, everything else
// forwarded to the launcher (→ MeshCore CLI under MESH_LOCK).
static void test_hook_serial_pump() {
    // 768: must hold a full "T:FDATA <base64>" line — 192 silently
    // truncated file-push chunks (found the hard way, 2026-07-22).
    static char buf[768];
    static size_t len = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            buf[len] = 0;
            if (len > 0) {
                if (buf[0] == 'T' && buf[1] == ':') handle_test_command(String(buf));
                else if (!strcmp(buf, "VERSION"))   handle_test_command(String(buf));
                else pyxis_host_serial_line(buf);
            }
            len = 0;
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}
#endif  // HYBRID_TEST_HOOKS

// ---------------------------------------------------------------------------
// Service task
// ---------------------------------------------------------------------------

// Apply + persist TCP config. Runs on the service task only (T:TCP and
// the SET_TCP command both land here).
static bool do_set_tcp(bool enabled, const char* host, uint16_t port) {
    if (enabled && (!host || !host[0] || port == 0)) return false;
    s_cfg.tcp_en = enabled;
    if (host) s_cfg.tcp_host = String(host).substring(0, 63);
    if (port) s_cfg.tcp_port = port;
    strlcpy(s_tcp_host_c, s_cfg.tcp_host.c_str(), sizeof(s_tcp_host_c));
    Preferences p;
    p.begin("pyxis", false);
    p.putBool("tcp_en", s_cfg.tcp_en);
    p.putString("tcp_host", s_cfg.tcp_host);
    p.putUShort("tcp_port", s_cfg.tcp_port);
    p.end();
    if (enabled) start_tcp_interface();
    return true;
}

// Apply + persist a new display name. Runs on the service task only.
static bool do_set_display_name(const char* name) {
    if (!name) return false;
    s_cfg.display_name = String(name).substring(0, 31);
    strlcpy(s_disp_name, s_cfg.display_name.c_str(), sizeof(s_disp_name));
    Preferences prefs;
    prefs.begin("pyxis", false);
    prefs.putString("disp_name", s_cfg.display_name);
    prefs.end();
    if (s_router) {
        s_router->set_display_name(s_cfg.display_name.c_str());
        s_router->announce();   // let peers pick the new name up now
    }
    return true;
}

static void svc_execute_cmd(SvcCmd* cmd) {
    switch (cmd->op) {
        case SvcCmd::SEND:
            cmd->ok = do_send(cmd->dest_hex, cmd->text, cmd->out_hash);
            break;
        case SvcCmd::ANNOUNCE:
            cmd->ok = (s_router != nullptr);
            if (s_router) s_router->announce();
            break;
        case SvcCmd::SET_NAME:
            cmd->ok = do_set_display_name(cmd->text);
            break;
        case SvcCmd::SET_TCP:
            cmd->ok = do_set_tcp(cmd->flag, cmd->text, cmd->port);
            break;
        case SvcCmd::LIST_CONVS: {
            // All Bytes/vector allocation stays HERE, on the pool's
            // single legal thread; the caller gets plain structs.
            // Rows go into the heap block's OUTPUT TAIL at (cmd + 1),
            // never through cmd->dest (see the SvcCmd::dest comment).
            cmd->rows = 0;
            cmd->ok = (s_store && cmd->max_rows > 0);
            if (!cmd->ok) break;
            auto* rows = (PyxisConvRow*)(cmd + 1);
            auto convs = s_store->get_conversations();   // newest first
            for (auto& p : convs) {
                if (cmd->rows >= cmd->max_rows) break;
                // ~8.3KB ConversationInfo by-value temporary on this
                // task's 16KB stack — sequential with loop steps, never
                // concurrent. Re-check svc_hwm in T:STATE after first
                // on-device use (wadamesh ran this on a 24KB stack).
                auto ci = s_store->get_conversation_info(p);
                PyxisConvRow& r = rows[cmd->rows++];
                strlcpy(r.peer_hex, p.toHex().c_str(), sizeof(r.peer_hex));
                strlcpy(r.name, ci.display_name, sizeof(r.name));
                r.unread = (uint16_t)ci.unread_count;
                r.msg_count = (uint16_t)ci.message_count;
                r.last_ts = (uint32_t)ci.last_activity;
                r.last_text[0] = 0;
                if (ci.message_count > 0) {
                    auto md = s_store->load_message_metadata(
                        ci.last_message_hash_bytes());
                    if (md.valid) {
                        strlcpy(r.last_text, md.content.c_str(),
                                sizeof(r.last_text));
                        if (md.timestamp > 0) r.last_ts = (uint32_t)md.timestamp;
                    }
                }
            }
            break;
        }
        case SvcCmd::READ_THREAD: {
            cmd->rows = 0;
            cmd->name_buf[0] = 0;
            RNS::Bytes peer;
            cmd->ok = (s_store && cmd->max_rows > 0 &&
                       parse_hex16(cmd->dest_hex, peer));
            if (!cmd->ok) break;
            auto* rows = (PyxisMsgRow*)(cmd + 1);
            auto ci = s_store->get_conversation_info(peer);
            strlcpy(cmd->name_buf, ci.display_name, sizeof(cmd->name_buf));
            size_t first = ci.message_count > (size_t)cmd->max_rows
                               ? ci.message_count - cmd->max_rows : 0;
            for (size_t i = first; i < ci.message_count &&
                                   cmd->rows < cmd->max_rows; ++i) {
                auto md = s_store->load_message_metadata(ci.message_hash_bytes(i));
                if (!md.valid) continue;
                PyxisMsgRow& r = rows[cmd->rows++];
                strlcpy(r.text, md.content.c_str(), sizeof(r.text));
                r.ts = (uint32_t)md.timestamp;
                r.incoming = md.incoming ? 1 : 0;
                r.state = (uint8_t)md.state;
            }
            break;
        }
        case SvcCmd::MARK_READ: {
            // Store writes stay on this task (UI opens a thread ->
            // unread count clears).
            RNS::Bytes peer;
            cmd->ok = (s_store && parse_hex16(cmd->dest_hex, peer));
            if (cmd->ok) s_store->mark_conversation_read(peer);
            break;
        }
    }
    // Completion signalling + freeing is the drain loop's job (ownership
    // handshake) — nothing more here.
}

static void svc_task_body(void*) {
    if (!service_init()) {
        s_running = false;
        vTaskDelete(nullptr);
        return;
    }
    s_running = true;
    // DIAGNOSTIC (2026-08-07 crash hunt): reset reason distinguishes
    // panic / task-WDT / int-WDT / brownout even when the USB console
    // dies before the backtrace can flush.
    Serial.printf("[pyxis] reset_reason=%d\n", (int)esp_reset_reason());
    Serial.printf("[pyxis] service up  id=%s  dest=%s\n",
                  s_identity_hex, s_dest_hex);

    bool     was_connected = false;
    bool     was_online = false;
    uint32_t last_disconnect_kick = 0;
    uint32_t last_announce = 0;
    uint32_t online_since = 0;
    uint8_t  announce_stage = 0;   // position in the early-announce ladder

    for (;;) {
        // -- WiFi watcher (D6). meshpunk's wifi_auto owns credentials
        // and association; we hold the link up and retry through the
        // host when it drops.
        bool connected = (WiFi.status() == WL_CONNECTED);
        if (connected && !was_connected) {
            WiFi.setAutoReconnect(true);   // pyxis:574 — in-RAM, no NVS wear
            WiFi.persistent(false);
            start_auto_interface();
            start_tcp_interface();
            ntp_kick();
        } else if (!connected && was_connected) {
            Serial.println("[pyxis] WiFi lost");
        }
        if (!connected && millis() - last_disconnect_kick > 30000) {
            last_disconnect_kick = millis();
            pyxis_host_wifi_kick();
        }
        was_connected = connected;

        // -- RNS/LXMF pumps (pyxis loop(): 2340, 2369, 2386-2389).
        s_reticulum->loop();
        if (s_tcp_if && s_tcp_impl) s_tcp_impl->loop();
        // pyxis never pumped AutoInterface explicitly (FINDINGS §3);
        // do it here — harmless if Transport also services it.
        if (s_auto_impl) s_auto_impl->loop();
        s_router->process_outbound();
        s_router->process_inbound();
        s_router->process_sync();
        pyxis_call_update();   // M3: call FSM + TX audio pump
        ntp_pump();

        // -- Online-state transitions → RNS_STATUS events + announces.
        bool online = (s_auto_if && s_auto_if->online()) ||
                      (s_tcp_if && s_tcp_if->online());
        if (online != was_online) {
            post_event(PyxisEvent::RNS_STATUS, nullptr, nullptr,
                       online ? "online" : "offline");
            if (online) {
                online_since = millis();
                announce_stage = 0;   // rerun the ladder after an outage
            }
            was_online = online;
        }
        // Announce ladder: online+10s, +60s, +5min, then every
        // announce_interval. The early repeats exist because
        // AutoInterface silently DROPS announces under internal-heap
        // pressure ("Skipping announce - low memory", observed on
        // device 2026-07-22 with BLE+WiFi+RNS coexisting) — a single
        // dropped first announce otherwise strands the device unseen
        // for a full hour. Repeated announces are cheap and normal on
        // RNS (desktop peers announce on the same order of frequency).
        if (online) {
            static const uint32_t LADDER_MS[] = {10000, 60000, 300000};
            uint32_t since_online = millis() - online_since;
            if (announce_stage < 3) {
                if (since_online >= LADDER_MS[announce_stage]) {
                    s_router->announce();
                    pyxis_call_announce();   // telephony dest rides the ladder
                    last_announce = millis();
                    announce_stage++;
                }
            } else if (millis() - last_announce >=
                       s_cfg.announce_interval * 1000UL) {
                s_router->announce();
                pyxis_call_announce();
                last_announce = millis();
            }
        }

        // -- Cross-task commands (heap-owned; see SvcCmd ownership note).
        SvcCmd* cmd = nullptr;
        while (s_cmd_q && xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE) {
            if (cmd->state.load(std::memory_order_acquire) == SvcCmd::CMD_ABANDONED) {
                heap_caps_free(cmd);   // caller gave up while we were busy
                continue;
            }
            svc_execute_cmd(cmd);
            uint8_t expect = SvcCmd::CMD_PENDING;
            if (cmd->state.compare_exchange_strong(expect, SvcCmd::CMD_DONE,
                                                   std::memory_order_acq_rel)) {
                xSemaphoreGive(cmd->done);   // caller still waiting; it frees
            } else {
                heap_caps_free(cmd);         // abandoned mid-execution
            }
        }

#ifdef HYBRID_TEST_HOOKS
        test_hook_serial_pump();
#endif

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool pyxis_service_start() {
    if (s_task) return false;
    load_settings();
    if (!s_cfg.rns_en) {
        Serial.println("[pyxis] disabled via NVS (pyxis/rns_en)");
        return false;
    }
    s_evq = xQueueCreate(24, sizeof(PyxisEvent));
    s_cmd_q = xQueueCreate(4, sizeof(SvcCmd*));
    // Static 16KB stack (was heap-allocated 24KB, matching pyxis's
    // loop budget). Trimmed 2026-07-22 on measurement: svc_hwm showed
    // ~3.5KB peak use through boot + messaging + announces; 16KB
    // leaves >4x margin plus call-time codec2-decode headroom. Static
    // BSS per the D11 doctrine — off the heap entirely, so it can
    // neither fail nor fragment. Re-check svc_hwm in T:STATE after the
    // first long call before trimming further.
    static StaticTask_t s_svc_tcb;
    // 16K→24K (backport 2026-08-07, wadamesh M3 on-device measurement:
    // the call path needs ~17-20K — codec2 3200 decode runs on THIS task
    // at the bottom of the RNS inbound chain; 16K blew the stack
    // watchpoint in lpc_post_filter the moment the first audio frame
    // arrived. Backtrace in TESTLOG "Live-bench session 2026-08-07".)
    static StackType_t  s_svc_stack[24576 / sizeof(StackType_t)];
    s_task = xTaskCreateStaticPinnedToCore(
        svc_task_body, "pyxis_svc",
        sizeof(s_svc_stack) / sizeof(StackType_t), nullptr,
        2, s_svc_stack, &s_svc_tcb, 1);
    return s_task != nullptr;
}

bool pyxis_service_running() { return s_running; }
QueueHandle_t pyxis_event_queue() { return s_evq; }
bool pyxis_wifi_hold() { return s_task != nullptr && s_cfg.rns_en; }

bool pyxis_get_identity_hash(char out_hex[33]) {
    if (!s_running) return false;
    strlcpy(out_hex, s_identity_hex, 33);
    return true;
}

bool pyxis_get_delivery_dest(char out_hex[33]) {
    if (!s_running) return false;
    strlcpy(out_hex, s_dest_hex, 33);
    return true;
}

// Marshal a command onto the service task. The caller's SvcCmd is a
// TEMPLATE: it is copied into a heap-owned block whose lifetime the
// ownership handshake governs; results copy back only on completion.
// out_bytes > 0 appends an OUTPUT TAIL to the block (LIST_CONVS/
// READ_THREAD row arrays): the service fills the tail, and the caller's
// tmpl.dest buffer is written only HERE on the DONE path — an abandoned
// command's tail dies with the block, never with the caller's stack.
// (Ops with an output tail carry no text; both live at (cmd + 1).)
static bool run_cmd(SvcCmd& tmpl, size_t out_bytes = 0) {
    if (!s_running || !s_cmd_q) return false;
    size_t text_len = tmpl.text ? strlen(tmpl.text) + 1 : 0;
    SvcCmd* cmd = (SvcCmd*)heap_caps_malloc(sizeof(SvcCmd) + text_len + out_bytes,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cmd) return false;
    memcpy((void*)cmd, (const void*)&tmpl, sizeof(SvcCmd));
    if (text_len) {
        char* t = (char*)(cmd + 1);
        memcpy(t, tmpl.text, text_len);
        cmd->text = t;
    }
    if (out_bytes) memset((void*)(cmd + 1), 0, out_bytes);
    cmd->state.store(SvcCmd::CMD_PENDING, std::memory_order_relaxed);
    cmd->done = xSemaphoreCreateBinaryStatic(&cmd->sem_buf);
    SvcCmd* p = cmd;
    if (xQueueSend(s_cmd_q, &p, pdMS_TO_TICKS(100)) != pdTRUE) {
        heap_caps_free(cmd);
        return false;
    }
    // 3500ms: FS-stall windows can exceed the old 2000ms — but
    // correctness no longer depends on the bound; a timeout now
    // abandons safely instead of leaving a live pointer to a dead stack.
    if (xSemaphoreTake(cmd->done, pdMS_TO_TICKS(3500)) != pdTRUE) {
        uint8_t expect = SvcCmd::CMD_PENDING;
        if (cmd->state.compare_exchange_strong(expect, SvcCmd::CMD_ABANDONED,
                                               std::memory_order_acq_rel)) {
            return false;   // service will free it when it drains/finishes
        }
        // Service completed in the race window — the Give is in flight.
        xSemaphoreTake(cmd->done, portMAX_DELAY);
    }
    memcpy((void*)&tmpl, (const void*)cmd, sizeof(SvcCmd));   // results back
    // DONE path only: the service finished and Gave — the block's
    // output tail is complete and quiescent, and the caller's buffer
    // is alive (we're on its stack frame). Copy out now, never earlier.
    if (out_bytes && tmpl.dest && tmpl.ok)
        memcpy(tmpl.dest, (const void*)(cmd + 1), out_bytes);
    heap_caps_free(cmd);
    return tmpl.ok;
}

bool pyxis_send_lxmf(const char* dest_hex, const char* text,
                     char out_msg_hash[33]) {
    SvcCmd cmd = {};
    cmd.op = SvcCmd::SEND;
    if (!dest_hex || strlen(dest_hex) != 32) return false;
    strlcpy(cmd.dest_hex, dest_hex, sizeof(cmd.dest_hex));
    cmd.text = text;
    if (!run_cmd(cmd)) return false;
    if (out_msg_hash) strlcpy(out_msg_hash, cmd.out_hash, 33);
    return true;
}

bool pyxis_announce() {
    SvcCmd cmd = {};
    cmd.op = SvcCmd::ANNOUNCE;
    return run_cmd(cmd);
}

bool pyxis_set_display_name(const char* name) {
    if (!name) return false;
    SvcCmd cmd = {};
    cmd.op = SvcCmd::SET_NAME;
    cmd.text = name;
    return run_cmd(cmd);
}

bool pyxis_get_display_name(char out[32]) {
    if (!out) return false;
    strlcpy(out, s_disp_name, 32);
    return true;
}

bool pyxis_set_tcp(bool enabled, const char* host, uint16_t port) {
    SvcCmd cmd = {};
    cmd.op = SvcCmd::SET_TCP;
    cmd.flag = enabled;
    cmd.text = host ? host : "";
    cmd.port = port;
    return run_cmd(cmd);
}

bool pyxis_set_auto_en(bool enabled) {
    // Plain bool + thread-safe NVS: no marshalling needed — the gate
    // is only consulted at (re)start, same reboot-to-apply semantics
    // as T:AUTO.
    s_cfg.auto_en = enabled;
    Preferences p;
    p.begin("pyxis", false);
    p.putBool("auto_en", enabled);
    p.end();
    return true;
}

bool pyxis_get_auto_en(bool* enabled, bool* running) {
    if (enabled) *enabled = s_cfg.auto_en;
    if (running) *running = (s_auto_if && s_auto_if->online());
    return true;
}

bool pyxis_get_tcp(bool* enabled, char host_out[64], uint16_t* port,
                   bool* online) {
    if (enabled) *enabled = s_cfg.tcp_en;
    if (host_out) strlcpy(host_out, s_tcp_host_c, 64);
    if (port) *port = s_cfg.tcp_port;
    if (online) *online = (s_tcp_if && s_tcp_if->online());
    return true;
}

// ---- persisted-conversation snapshots (see PyxisService.h block
// comment for provenance + the RNS-types-stay-on-svc-task hard rule).
// max_rows clamps also bound the PSRAM output-tail allocation.
int pyxis_list_conversations(PyxisConvRow* out, int max_rows) {
    if (!out || max_rows <= 0) return 0;
    if (max_rows > (int)LXMF::MAX_CONVERSATIONS)
        max_rows = (int)LXMF::MAX_CONVERSATIONS;
    SvcCmd cmd = {};
    cmd.op = SvcCmd::LIST_CONVS;
    cmd.dest = out;
    cmd.max_rows = max_rows;
    if (!run_cmd(cmd, (size_t)max_rows * sizeof(PyxisConvRow))) return 0;
    return cmd.rows;
}

int pyxis_read_thread(const char* peer_hex, PyxisMsgRow* out, int max_rows,
                      char name_out[48]) {
    if (!peer_hex || strlen(peer_hex) != 32 || !out || max_rows <= 0)
        return -1;
    if (max_rows > (int)LXMF::MAX_MESSAGES_PER_CONVERSATION)
        max_rows = (int)LXMF::MAX_MESSAGES_PER_CONVERSATION;
    SvcCmd cmd = {};
    cmd.op = SvcCmd::READ_THREAD;
    cmd.dest = out;
    cmd.max_rows = max_rows;
    strlcpy(cmd.dest_hex, peer_hex, sizeof(cmd.dest_hex));
    if (!run_cmd(cmd, (size_t)max_rows * sizeof(PyxisMsgRow))) return -1;
    if (name_out) strlcpy(name_out, cmd.name_buf, 48);
    return cmd.rows;
}

bool pyxis_mark_read(const char* peer_hex) {
    if (!peer_hex || strlen(peer_hex) != 32) return false;
    SvcCmd cmd = {};
    cmd.op = SvcCmd::MARK_READ;
    strlcpy(cmd.dest_hex, peer_hex, sizeof(cmd.dest_hex));
    return run_cmd(cmd);
}

#ifdef HYBRID_TEST_HOOKS
// Give the drain (or future M4 dispatch) a way to feed the T:RX ring:
// main.cpp's drain_rns_events calls this for MSG_RECEIVED events.
void pyxis_test_record_rx_event(const PyxisEvent& ev) {
    test_hook_record_rx(ev);
}
#endif
