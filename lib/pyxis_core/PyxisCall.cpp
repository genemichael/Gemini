// SPDX-License-Identifier: GPL-3.0-or-later
//
// LXST call engine — see PyxisCall.h. State machine, wire format, and
// timeouts ported 1:1 from pyxis UIManager.cpp ("UIM:<line>"); the
// LVGL side effects become CALL_* events, and the audio output path is
// meshpunk's mixer (HYBRID_PLAN D4) instead of pyxis's I2SPlayback.
//
// Threading:
//  - Everything RNS (link callbacks, packets, FSM, TX pump) runs on the
//    pyxis_svc task — Reticulum::loop() fires callbacks there, and
//    pyxis_call_update() runs there, so unlike pyxis there is no
//    cross-thread signal marshalling needed; the signal queue is kept
//    anyway to avoid re-entrant FSM transitions from inside packet
//    callbacks (same reason pyxis queues them; UIM:1241-1244).
//  - ES7210 register I/O executes on core 0 via the s_es_req flag
//    drained by pyxis_call_core0_service() in loop() (HYBRID_PLAN D5).
//  - The mixer pull callback runs on meshpunk's sound_task (core 1),
//    reading a PSRAM PCM ring filled by the decoder on the svc task.
//  - Control API marshals through a pending-op flag consumed by
//    pyxis_call_update() (mirrors pyxis's _call_answer_pending).

#include "PyxisCall.h"
#include "pyxis_internal.h"

#include <Wire.h>
#include <esp_heap_caps.h>

#include <microReticulum/Identity.h>
#include <microReticulum/Destination.h>
#include <microReticulum/Transport.h>
#include <microReticulum/Link.h>
#include <microReticulum/Packet.h>
#include <LXMF/LXMRouter.h>

#include "i2s_capture.h"
#include "codec_wrapper.h"
#include "es7210.h"
#include "audio_hal.h"
#include <driver/i2s.h>   // i2s_start/i2s_stop on the pre-installed mic port

// meshpunk mixer pull API — C++ linkage matching src/sound.h's
// declaration (declared here rather than including src/ headers into
// the lib; the signature is part of the launcher's stable surface).
void sound_extern_set_pull(void (*cb)(int16_t* out, int count), int sample_rate);

// lxst_audio's capture loop logs through pyxis's UDP logger; pyxis
// defined it in main.cpp (pyxis:195). We don't port UDP logging —
// serial passthrough keeps the diagnostics without the lwIP stack cost.
extern "C" void pyxis_log(const char* msg) {
    Serial.println(msg);
}
// codec2 modes (sh123/esp32_codec2 codec2.h)
#include <codec2.h>

using namespace RNS;

// ── LXST constants (UIM.h:334-349) ─────────────────────────────────────────
static constexpr uint8_t LXST_STATUS_BUSY        = 0x00;
static constexpr uint8_t LXST_STATUS_REJECTED    = 0x01;
static constexpr uint8_t LXST_STATUS_AVAILABLE   = 0x03;
static constexpr uint8_t LXST_STATUS_RINGING     = 0x04;
static constexpr uint8_t LXST_STATUS_CONNECTING  = 0x05;
static constexpr uint8_t LXST_STATUS_ESTABLISHED = 0x06;
static constexpr uint8_t LXST_CODEC_CODEC2 = 0x02;
static constexpr int LXST_PREFERRED_PROFILE = 0xFF;
static constexpr int LXST_PROFILE_ULBW = 0x10;
static constexpr int LXST_PROFILE_VLBW = 0x20;
static constexpr int LXST_PROFILE_LBW  = 0x30;

static int profile_to_codec2_mode(int profile) {   // UIM.cpp:51-58
    switch (profile) {
        case LXST_PROFILE_ULBW: return CODEC2_MODE_700C;
        case LXST_PROFILE_VLBW: return CODEC2_MODE_1600;
        case LXST_PROFILE_LBW:  return CODEC2_MODE_3200;
        default: return -1;
    }
}

enum class CallState : uint8_t {                   // UIM.h:361-371
    IDLE, PATH_REQUESTING, LINK_ESTABLISHING, WAIT_AVAILABLE,
    WAIT_RINGING, RINGING, INCOMING_RINGING, CONNECTING, ACTIVE
};

static const char* state_name(CallState s) {
    switch (s) {
        case CallState::IDLE:              return "IDLE";
        case CallState::PATH_REQUESTING:   return "PATH_REQUESTING";
        case CallState::LINK_ESTABLISHING: return "LINK_ESTABLISHING";
        case CallState::WAIT_AVAILABLE:    return "WAIT_AVAILABLE";
        case CallState::WAIT_RINGING:      return "WAIT_RINGING";
        case CallState::RINGING:           return "RINGING";
        case CallState::INCOMING_RINGING:  return "INCOMING_RINGING";
        case CallState::CONNECTING:        return "CONNECTING";
        case CallState::ACTIVE:            return "ACTIVE";
    }
    return "UNKNOWN";
}

// ── state ──────────────────────────────────────────────────────────────────
static Destination s_lxst_dest{Type::NONE};
static Link  s_call_link{Type::NONE};     // NONE ctor: UIM.h:376-379
static bool  s_call_active_obj = false;   // link object holds a real link
static CallState s_state = CallState::IDLE;
static Bytes s_peer_hash;                 // caller/callee identity hash
static Bytes s_dest_hash;                 // peer lxst destination hash
static uint32_t s_start_ms = 0;
static uint32_t s_timeout_ms = 0;
static bool s_muted = false;
static int  s_preferred_profile = LXST_PROFILE_LBW;    // was ULBW (UIM.cpp:23-27);
    // 3200 default per Gene 2026-08-07: matches wadamesh, decodable by
    // both embedded RX paths (PROFILE_AUDIT.md §4 — a 10-frame 700C
    // batch overflows every embedded decode buffer), ~10x cheaper CPU.
// Profile negotiation state (PROFILE_AUDIT.md §6, with the 3200-only
// adoption clamp per Gene 2026-08-07): the peer's announced profile,
// adopted only if it is LBW/3200 — the sole profile decodable within
// the pcm[2048] RX scratch (a 10-frame 700C batch decodes to 3200
// samples, §4b; 40ms modes are also the WDT territory of §4d). -1 =
// nothing adoptable received; both reset per-call in call_ended().
static int  s_remote_profile = -1;
static bool s_profile_acked = false;   // reply-at-most-once guard (§2 storm kill)

// Effective profile for this call: the adopted remote if one was
// accepted, else our own preference. Used for codec init and for the
// profile signals we send — echoing the effective value (never
// restating our own against an adopted one) is what terminates the
// profile-signal exchange (§6).
static int effective_profile() {
    return (s_remote_profile >= 0) ? s_remote_profile : s_preferred_profile;
}

static uint32_t s_tx_count = 0, s_rx_count = 0;
static volatile bool s_link_closed_pending = false;

// Signal queue (UIM.h:387-390)
static constexpr int SIGQ = 8;
static volatile uint8_t s_sigq[SIGQ];
static volatile uint8_t s_sigq_w = 0, s_sigq_r = 0;

// Pending control ops from other tasks, consumed in pyxis_call_update()
static volatile bool s_answer_pending = false;
static volatile bool s_hangup_pending = false;
static char s_initiate_hex[33] = {0};
static volatile bool s_initiate_pending = false;

// ── audio ──────────────────────────────────────────────────────────────────
static I2SCapture*    s_capture = nullptr;
static Codec2Wrapper* s_enc = nullptr;
static Codec2Wrapper* s_dec = nullptr;

// Decoded-PCM ring (8kHz mono) feeding the mixer pull callback.
// 16384 samples ≈ 2s; PSRAM so it never fights internal SRAM.
static constexpr int PCM_RING = 16384;
static int16_t* s_pcm_ring = nullptr;
static volatile int s_pcm_w = 0, s_pcm_r = 0;
static volatile bool s_playing = false;      // prebuffer passed, feeding mixer
static int s_prebuffer_samples = 4800;       // ~600ms @8k (≈ pyxis's 15 frames)
static uint32_t s_underruns = 0;
static float s_resample_phase = 0.0f;
static int16_t s_last_sample = 0;

static inline int pcm_buffered() {
    int w = s_pcm_w, r = s_pcm_r;
    return (w - r + PCM_RING) % PCM_RING;
}

// Mixer pull callback — runs on sound_task (core 1). The mixer is told
// we produce 11025Hz (its integer-upsample ceiling, ×4 → 44.1k); we
// linearly resample our 8kHz ring by 8000/11025 per output sample.
static void mixer_pull_cb(int16_t* out, int count) {
    constexpr float STEP = 8000.0f / 11025.0f;
    for (int i = 0; i < count; i++) {
        // Re-arm on drain (PROFILE_AUDIT.md §4d; Columba garble,
        // TESTLOG Live-bench 2026-08-07 underrun=27807/~40s): when the
        // ring starves mid-playback, drop back behind the prebuffer
        // gate instead of zero-stuffing per sample — the decoder path
        // re-sets s_playing once s_prebuffer_samples refill, so jitter
        // gaps become brief pauses rather than per-sample chatter.
        // NOTE: s_underruns now counts drain EVENTS (one per re-arm),
        // no longer starved output samples.
        if (s_playing && pcm_buffered() < 2) {
            s_playing = false;
            s_underruns++;
        }
        if (!s_playing) {
            out[i] = 0;
            continue;
        }
        int r = s_pcm_r;
        int16_t a = s_pcm_ring[r];
        int16_t b = s_pcm_ring[(r + 1) % PCM_RING];
        float t = s_resample_phase;
        out[i] = (int16_t)(a + (b - a) * t);
        s_resample_phase += STEP;
        if (s_resample_phase >= 1.0f) {
            s_resample_phase -= 1.0f;
            s_pcm_r = (r + 1) % PCM_RING;
            s_last_sample = a;
        }
    }
}

// ES7210 bring-up marshalled to core 0 (D5). Stages: 1 = full init
// (pre-clock), 2 = ctrl_state restart (post-clock). UIM has these
// inline in LXSTAudio::init (lxst_audio.cpp:38-105, excluded file).
static volatile int  s_es_req = 0;      // stage requested by svc task
static volatile bool s_es_done = false;
static volatile bool s_es_ok = false;
static uint8_t s_es_gain = 30;          // GAIN_30DB default (pyxis micGain)

void pyxis_call_core0_service() {
    int stage = s_es_req;
    if (!stage) return;
    s_es_req = 0;
    uint32_t rv = ESP_OK;
    if (stage == 1) {
        audio_hal_codec_config_t cfg = {};
        cfg.adc_input = AUDIO_HAL_ADC_INPUT_ALL;
        cfg.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;
        cfg.i2s_iface.mode = AUDIO_HAL_MODE_SLAVE;
        cfg.i2s_iface.fmt = AUDIO_HAL_I2S_NORMAL;
        cfg.i2s_iface.samples = AUDIO_HAL_08K_SAMPLES;
        cfg.i2s_iface.bits = AUDIO_HAL_BIT_LENGTH_16BITS;
        rv |= es7210_adc_init(&Wire, &cfg);
        rv |= es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
        rv |= es7210_adc_set_gain(
            (es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2 |
                                  ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4),
            (es7210_gain_value_t)s_es_gain);
        rv |= es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);
    } else {
        audio_hal_codec_config_t cfg2 = {};
        cfg2.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;
        rv |= es7210_adc_ctrl_state(cfg2.codec_mode, AUDIO_HAL_CTRL_START);
    }
    s_es_ok = (rv == ESP_OK);
    s_es_done = true;
}

// Request an ES7210 stage from the svc task and wait for core 0 to run
// it (loop() drains every tick; 500ms is generous).
static bool es7210_stage(int stage) {
    s_es_done = false;
    s_es_req = stage;
    uint32_t t0 = millis();
    while (!s_es_done && millis() - t0 < 500) vTaskDelay(pdMS_TO_TICKS(5));
    if (!s_es_done) { s_es_req = 0; return false; }
    return true;   // rv warnings are non-fatal in pyxis too (lxst_audio.cpp:59)
}

static void post_state_event() {
    pyxis_internal_post_event(PyxisEvent::CALL_STATE,
                              s_peer_hash ? s_peer_hash.toHex().c_str() : "",
                              nullptr, state_name(s_state));
}

// ── audio pipeline lifecycle (replaces LXSTAudio init/startFullDuplex) ─────
//
// Pre-allocation doctrine (2026-07-22, after two call-time allocation
// deaths on device): every internal-RAM resource the pipeline needs is
// claimed ONCE at boot, while the heap is one contiguous block —
// I2S driver + DMA buffers (persistent across calls), the capture
// task stack (static BSS), the PCM ring (PSRAM). Per-call work is
// PSRAM-only (codec instances via the memtools patch, capture buffers
// upstream-PSRAM already) plus I2C register writes. Starting a call
// allocates approximately nothing from internal heap.

// Per-stage internal-heap logging: turns any allocation failure during
// call setup into a named, quantified report. The dma=free/largest
// columns track MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL separately: WiFi RX
// can exhaust the DMA-capable pool while generic-internal looks
// healthy (WADAMESH_BACKPORT_BRIEF.md Addendum §4; format matches
// wadamesh's log_int_heap).
static void log_int_heap(const char* stage) {
    Serial.printf("[call][mem] %-12s int free=%u largest=%u dma=%u/%u\n", stage,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    // DIAGNOSTIC (2026-08-07, answer-time crash hunt): synchronous drain so
    // each stage line reaches the host BEFORE the next stage runs — a panic
    // otherwise loses the 8KB TX buffer and with it the crash bracket.
    Serial.flush();
}

// DIAGNOSTIC (same hunt): flushed breadcrumb for non-stage checkpoints.
static void diag_mark(const char* what) {
    Serial.printf("[call][diag] %s\n", what);
    Serial.flush();
}

// Boot-time reservation. Runs on the service task during service init;
// failure leaves s_capture null and calls will refuse cleanly.
static void audio_preallocate() {
    log_int_heap("prealloc in");
    if (!s_pcm_ring) {
        s_pcm_ring = (int16_t*)heap_caps_malloc(PCM_RING * sizeof(int16_t),
                                                MALLOC_CAP_SPIRAM);
    }
    s_capture = new I2SCapture();
    s_capture->setPersistent(true);
    if (!s_capture->init()) {
        Serial.println("[call] PREALLOC FAILED: I2S driver install");
        delete s_capture;
        s_capture = nullptr;
        return;
    }
    i2s_stop(I2S_NUM_1);   // idle the clocks until a call starts
    log_int_heap("prealloc out");
    Serial.println("[call] audio pipeline pre-allocated (persistent I2S)");
}

static void audio_stop_and_free() {
    s_playing = false;
    sound_extern_set_pull(nullptr, 0);
    if (s_capture) {
        s_capture->stop();            // persistent: task + clocks only
        s_capture->releaseBuffers();  // per-call PSRAM buffers
    }
    if (s_enc) { s_enc->destroy(); delete s_enc; s_enc = nullptr; }
    if (s_dec) { s_dec->destroy(); delete s_dec; s_dec = nullptr; }
    s_pcm_w = s_pcm_r = 0;
    s_resample_phase = 0.0f;
}

static bool audio_start(int codec_mode) {
    log_int_heap("start");
    if (!s_capture || !s_pcm_ring) {
        Serial.println("[call] audio not pre-allocated, cannot start");
        return false;
    }
    s_pcm_w = s_pcm_r = 0;
    s_playing = false;
    s_underruns = 0;

    // ES7210 pre-clock init (core 0) → I2S clocks on → ES7210 restart
    // with clocks live. Sequence per lxst_audio.cpp; the driver is
    // already installed, so this is register I/O only.
    if (!es7210_stage(1)) { Serial.println("[call] ES7210 init timeout"); }
    i2s_start(I2S_NUM_1);
    es7210_stage(2);
    log_int_heap("i2s+es7210");

    s_enc = new Codec2Wrapper();
    if (!s_enc->create(codec_mode)) {
        Serial.println("[call] codec2 encoder create FAILED");
        audio_stop_and_free();
        return false;
    }
    log_int_heap("codec2 enc");
    s_dec = new Codec2Wrapper();
    if (!s_dec->create(codec_mode)) {
        Serial.println("[call] codec2 decoder create FAILED");
        audio_stop_and_free();
        return false;
    }
    log_int_heap("codec2 dec");
    if (!s_capture->configureEncoder(s_enc, true)) {
        audio_stop_and_free();
        return false;
    }
    log_int_heap("enc config");
    s_capture->setMute(s_muted);
    if (!s_capture->start()) {
        Serial.printf("[call] capture start FAILED (int free=%u largest=%u dma=%u/%u)\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        audio_stop_and_free();
        return false;
    }
    // Speaker side: hand the mixer our pull callback (D4). Prebuffer
    // gates actual playback inside the callback.
    sound_extern_set_pull(mixer_pull_cb, 11025);
    Serial.println("[call] audio pipeline up (capture + mixer pull)");
    return true;
}

// ── wire TX (UIM:1094-1204) ────────────────────────────────────────────────
static void call_send_signal(int signal) {                 // UIM:1094
    if (!s_call_active_obj || s_call_link.status() != Type::Link::ACTIVE) return;
    uint8_t b[7];
    int len;
    b[0] = 0x81; b[1] = 0x00; b[2] = 0x91;
    if (signal <= 0x7F) { b[3] = (uint8_t)signal; len = 4; }
    else if (signal <= 0xFF) { b[3] = 0xCC; b[4] = (uint8_t)signal; len = 5; }
    else { b[3] = 0xCD; b[4] = (uint8_t)(signal >> 8); b[5] = (uint8_t)signal; len = 6; }
    try {
        Bytes data(b, len);
        Packet pkt(s_call_link, data);
        pkt.send();
    } catch (const std::exception& e) {
        Serial.printf("[call] signal send exception: %s\n", e.what());
    }
}

static void call_send_audio_batch(const uint8_t* batch, int batch_len) {  // UIM:1138
    if (!s_call_active_obj || s_call_link.status() != Type::Link::ACTIVE) return;
    // {0x01: bin8(batch)} — batch = [0x02 codec_type][mode][10*frames]
    uint8_t p[256];
    int pos = 0;
    p[pos++] = 0x81; p[pos++] = 0x01;
    p[pos++] = 0xC4; p[pos++] = (uint8_t)batch_len;
    if (batch_len > (int)sizeof(p) - pos) return;
    memcpy(p + pos, batch, batch_len);
    pos += batch_len;
    try {
        Bytes data(p, pos);
        Packet pkt(s_call_link, data);
        pkt.send();
    } catch (const std::exception& e) {
        Serial.printf("[call] TX exception: %s\n", e.what());
    }
}

// ── RX path (UIM:1206-1379) ────────────────────────────────────────────────
static void call_rx_audio_frame(const uint8_t* frame, size_t frame_len) {  // UIM:1206
    if (!s_dec || s_state == CallState::IDLE) return;
    if (frame[0] != LXST_CODEC_CODEC2) return;
    static int16_t pcm[2048];
    int n = s_dec->decode(frame + 1, (int)frame_len - 1, pcm, 2048);
    if (n <= 0) return;
    for (int i = 0; i < n; i++) {
        int next = (s_pcm_w + 1) % PCM_RING;
        if (next == s_pcm_r) break;   // full — drop newest
        s_pcm_ring[s_pcm_w] = pcm[i];
        s_pcm_w = next;
    }
    if (!s_playing && pcm_buffered() >= s_prebuffer_samples) s_playing = true;
    s_rx_count++;
}

static void call_on_packet(const Bytes& data) {            // UIM:1241
    if (data.size() < 4) return;
    const uint8_t* buf = data.data();
    if (buf[0] != 0x81) return;
    uint8_t field = buf[1];

    if (field == 0x00) {
        if (buf[2] != 0x91) return;
        int signal = -1;
        if (buf[3] <= 0x7F) signal = buf[3];
        else if (buf[3] == 0xCC && data.size() >= 5) signal = buf[4];
        else if (buf[3] == 0xCD && data.size() >= 6)
            signal = ((int)buf[4] << 8) | buf[5];
        if (signal < 0) return;
        if (signal >= LXST_PREFERRED_PROFILE) {            // UIM:1294-1303
            // Negotiation + storm kill (PROFILE_AUDIT.md §6, 3200-only
            // adoption clamp per Gene 2026-08-07). The old reply-always
            // branch echoed between two firmware engines for the whole
            // call, flooding the 8-deep signal queue — answer signals
            // drowned, caller stuck at RINGING while the callee went
            // ACTIVE (TESTLOG Live-bench 2026-08-07). Adopt LBW/3200
            // only (never 40ms modes — pcm[2048] scratch + WDT, §4);
            // reply AT MOST ONCE per call with the EFFECTIVE profile,
            // so even against an unpatched echo-always peer every
            // exchange is bounded (it replies to our single ack; we
            // never reply again).
            int remote = signal - LXST_PREFERRED_PROFILE;
            if (remote == LXST_PROFILE_LBW) s_remote_profile = remote;
            if (!s_profile_acked) {
                s_profile_acked = true;
                call_send_signal(LXST_PREFERRED_PROFILE + effective_profile());
            }
            return;
        }
        uint8_t w = s_sigq_w, next = (w + 1) % SIGQ;
        if (next != s_sigq_r) { s_sigq[w] = (uint8_t)signal; s_sigq_w = next; }
    } else if (field == 0x01) {                            // UIM:1321-1378
        if ((s_state != CallState::ACTIVE && s_state != CallState::CONNECTING))
            return;
        uint8_t fmt = buf[2];
        if ((fmt & 0xF0) == 0x90) {
            int array_len = fmt & 0x0F;
            size_t pos = 3;
            for (int i = 0; i < array_len; i++) {
                if (pos >= data.size()) break;
                size_t flen, fstart;
                if (buf[pos] == 0xC4) {
                    if (pos + 1 >= data.size()) break;
                    flen = buf[pos + 1]; fstart = pos + 2;
                } else if (buf[pos] == 0xC5) {
                    if (pos + 2 >= data.size()) break;
                    flen = ((size_t)buf[pos + 1] << 8) | buf[pos + 2];
                    fstart = pos + 3;
                } else break;
                if (fstart + flen > data.size() || flen < 2) break;
                call_rx_audio_frame(buf + fstart, flen);
                pos = fstart + flen;
            }
        } else if (fmt == 0xC4) {
            if (data.size() < 5) return;
            size_t flen = buf[3];
            if (data.size() < 4 + flen || flen < 2) return;
            call_rx_audio_frame(buf + 4, flen);
        } else if (fmt == 0xC5) {
            if (data.size() < 6) return;
            size_t flen = ((size_t)buf[3] << 8) | buf[4];
            if (data.size() < 5 + flen || flen < 2) return;
            call_rx_audio_frame(buf + 5, flen);
        }
    }
}

// ── teardown (UIM:1494-1523) ───────────────────────────────────────────────
static void call_ended(bool missed = false) {
    Serial.println("[call] ended");
    CallState prior = s_state;
    s_state = CallState::IDLE;
    audio_stop_and_free();
    if (s_call_active_obj) {
        s_call_link.teardown();
        s_call_link = Link(Type::NONE);
        s_call_active_obj = false;
    }
    if (missed && prior == CallState::INCOMING_RINGING) {
        pyxis_internal_post_event(PyxisEvent::MISSED_CALL,
                                  s_peer_hash ? s_peer_hash.toHex().c_str() : "",
                                  nullptr, nullptr);
    }
    post_state_event();   // "IDLE"
    s_peer_hash = Bytes();
    s_dest_hash = Bytes();
    s_remote_profile = -1;      // per-call negotiation state (§6)
    s_profile_acked = false;
    s_timeout_ms = 0;
}

// ── link callbacks (UIM:1690-1788) — fire on the svc task ─────────────────
static void on_call_link_established(Link& link) {         // UIM:1690
    Serial.println("[call] outgoing link established");
    s_call_link = link;
    s_call_active_obj = true;
    s_call_link.set_packet_callback(
        [](const Bytes& plaintext, const Packet&) { call_on_packet(plaintext); });
    s_call_link.set_link_closed_callback(
        [](Link& l) {
            if (s_state != CallState::IDLE) s_link_closed_pending = true;
        });
    s_state = CallState::WAIT_AVAILABLE;
    s_timeout_ms = millis() + 10000;
    post_state_event();
}

static void on_lxst_link_established(Link& link) {         // UIM:1734
    Serial.println("[call] incoming link established");
    if (s_state != CallState::IDLE) {                      // busy: UIM:1740-1749
        uint8_t busy[4] = {0x81, 0x00, 0x91, LXST_STATUS_BUSY};
        try {
            Bytes bd(busy, 4);
            Packet pkt(link, bd);
            pkt.send();
        } catch (...) {}
        link.teardown();
        return;
    }
    s_call_link = link;
    s_call_active_obj = true;
    s_muted = false;
    call_send_signal(LXST_STATUS_AVAILABLE);
    link.set_remote_identified_callback(
        [](const Link&, const Identity& identity) {        // UIM:1767
            s_peer_hash = identity.hash();
            s_call_link.set_packet_callback(
                [](const Bytes& plaintext, const Packet&) { call_on_packet(plaintext); });
            call_send_signal(LXST_STATUS_RINGING);
            s_state = CallState::INCOMING_RINGING;
            s_timeout_ms = millis() + 60000;
            pyxis_internal_post_event(PyxisEvent::CALL_INCOMING,
                                      s_peer_hash.toHex().c_str(), nullptr,
                                      nullptr);
            post_state_event();
        });
    link.set_link_closed_callback(
        [](Link& l) {
            if (s_state != CallState::IDLE) s_link_closed_pending = true;
        });
}

// ── FSM: signal processing (UIM:1382-1492) ─────────────────────────────────
static void call_process_signal(uint8_t signal) {
    Serial.printf("[call] signal 0x%02X in %s\n", signal, state_name(s_state));
    switch (s_state) {
        case CallState::WAIT_AVAILABLE:
            if (signal == LXST_STATUS_AVAILABLE) {
                s_call_link.identify(pyxis_internal_router()->identity());
                s_state = CallState::WAIT_RINGING;
                s_timeout_ms = millis() + 15000;
                post_state_event();
            } else if (signal == LXST_STATUS_BUSY) call_ended();
            break;
        case CallState::WAIT_RINGING:
            if (signal == LXST_STATUS_RINGING) {
                call_send_signal(LXST_PREFERRED_PROFILE + s_preferred_profile);
                s_state = CallState::RINGING;
                s_timeout_ms = millis() + 60000;
                post_state_event();
            } else if (signal == LXST_STATUS_BUSY ||
                       signal == LXST_STATUS_REJECTED) call_ended();
            break;
        case CallState::RINGING:
            if (signal == LXST_STATUS_CONNECTING) {
                diag_mark("answer: CONNECTING rx, pre audio_start");
                s_state = CallState::CONNECTING;
                post_state_event();
                // §6: the callee's profile signal may already have
                // arrived — use the converged (effective) profile.
                int mode = profile_to_codec2_mode(effective_profile());
                if (mode < 0) mode = CODEC2_MODE_700C;
                if (!audio_start(mode)) { call_ended(); return; }
                diag_mark("answer: audio_start ok (CONNECTING)");
            } else if (signal == LXST_STATUS_ESTABLISHED) {
                diag_mark("answer: ESTABLISHED rx, pre audio_start");
                s_state = CallState::ACTIVE;
                s_start_ms = millis();
                post_state_event();
                if (!s_capture) {
                    int mode = profile_to_codec2_mode(effective_profile());  // §6
                    if (mode < 0) mode = CODEC2_MODE_700C;
                    if (!audio_start(mode)) { call_ended(); return; }
                }
                diag_mark("answer: ACTIVE entered");
            } else if (signal == LXST_STATUS_REJECTED) call_ended();
            break;
        case CallState::CONNECTING:
            if (signal == LXST_STATUS_ESTABLISHED) {
                s_state = CallState::ACTIVE;
                s_start_ms = millis();
                post_state_event();
            }
            break;
        default: break;
    }
}

// ── TX pump (UIM:1525-1559) ────────────────────────────────────────────────
static void pump_call_tx() {
    if (s_state == CallState::IDLE) return;
    if (!s_capture || !s_capture->isCapturing()) return;
    if (!s_call_active_obj || s_call_link.status() != Type::Link::ACTIVE) return;
    int available = s_capture->availablePackets();
    while (available > 0) {
        uint8_t enc_buf[128];
        int enc_len = 0;
        if (!s_capture->readEncodedPacket(enc_buf, sizeof(enc_buf), &enc_len)) break;
        if (enc_len < 2) { available--; continue; }
        uint8_t batch[128];
        batch[0] = LXST_CODEC_CODEC2;
        memcpy(batch + 1, enc_buf, enc_len);
        call_send_audio_batch(batch, 1 + enc_len);
        s_tx_count++;
        available--;
    }
}

// ── per-tick update (UIM:1561-1686 minus LVGL) ─────────────────────────────
void pyxis_call_update() {
    uint32_t now = millis();

    if (s_link_closed_pending) {
        s_link_closed_pending = false;
        call_ended();
        return;
    }
    while (s_sigq_r != s_sigq_w) {
        uint8_t sig = s_sigq[s_sigq_r];
        s_sigq_r = (s_sigq_r + 1) % SIGQ;
        call_process_signal(sig);
        if (s_state == CallState::IDLE) return;
    }
    if (s_hangup_pending) {                                // UIM:1052-1084
        s_hangup_pending = false;
        if (s_state != CallState::IDLE) {
            if (s_state == CallState::INCOMING_RINGING)
                call_send_signal(LXST_STATUS_REJECTED);
            call_ended();
        }
        return;
    }
    if (s_answer_pending) {                                // UIM:1790-1843
        s_answer_pending = false;
        if (s_state == CallState::INCOMING_RINGING) {
            s_rx_count = 0;
            s_tx_count = 0;
            s_state = CallState::CONNECTING;
            post_state_event();
            call_send_signal(LXST_STATUS_CONNECTING);
            // §6: the caller's RINGING-time profile announce precedes
            // the human answering, so s_remote_profile is populated
            // here (if adoptable). Init the codec from the converged
            // profile and announce that same value — not a restatement
            // of our own preference.
            int eff = effective_profile();
            int mode = profile_to_codec2_mode(eff);
            if (mode < 0) mode = CODEC2_MODE_700C;
            if (!audio_start(mode)) { call_ended(); return; }
            call_send_signal(LXST_PREFERRED_PROFILE + eff);
            call_send_signal(LXST_STATUS_ESTABLISHED);
            s_state = CallState::ACTIVE;
            s_start_ms = millis();
            post_state_event();
        }
    }
    if (s_initiate_pending) {                              // UIM:917-996
        s_initiate_pending = false;
        if (s_state == CallState::IDLE) {
            // Pre-flight heap gate, re-derived 2026-07-22 from pyxis's
            // `free_heap < 40000` (UIM commit 8f265da). Torlando's 40KB
            // budgeted pyxis's DYNAMIC call costs: ~10KB link crypto +
            // 24KB capture-task stack + 8KB playback-task stack. In
            // this build the capture stack is static (BSS) and there is
            // no playback task (mixer path, D4), so the remaining
            // dynamic need is crypto + 2x codec2 instances + I2S DMA
            // buffers. Gate on the largest contiguous block too —
            // fragmentation, not total free, is what actually kills
            // allocations here (observed: 28KB free / 18KB largest).
            // Floors re-derived again after the pre-allocation rework:
            // the call path now allocates only link-crypto transients
            // (~10KB, freed after establishment) from internal heap —
            // codec2 is PSRAM (memtools patch), DMA + capture stack are
            // boot-reserved. 14KB/6KB covers crypto plus margin; this
            // guards crash-safety only, never encryption strength.
            size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            if (free_heap < 14000 || largest < 6000) {
                Serial.printf("[call] insufficient heap (free=%u largest=%u dma=%u/%u), aborting\n",
                              (unsigned)free_heap, (unsigned)largest,
                              (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            } else {
                Bytes peer;
                const char* hex = s_initiate_hex;
                for (int i = 0; i + 1 < 32; i += 2) {
                    char bb[3] = {hex[i], hex[i + 1], 0};
                    peer << (uint8_t)strtoul(bb, nullptr, 16);
                }
                Identity peer_identity = Identity::recall(peer);
                if (!peer_identity) {
                    Serial.println("[call] peer identity unknown");
                    pyxis_internal_post_event(PyxisEvent::CALL_STATE, "",
                                              nullptr, "IDLE");
                } else {
                    Destination peer_dest(peer_identity, Type::Destination::OUT,
                                          Type::Destination::SINGLE,
                                          "lxst", "telephony");
                    s_peer_hash = peer;
                    s_dest_hash = peer_dest.hash();
                    s_muted = false;
                    s_rx_count = 0;
                    s_tx_count = 0;
                    s_link_closed_pending = false;
                    s_sigq_w = s_sigq_r = 0;
                    if (Transport::has_path(s_dest_hash)) {
                        s_call_link = Link(peer_dest, on_call_link_established,
                                           [](Link& l) {
                            if (s_state != CallState::IDLE)
                                s_link_closed_pending = true;
                        });
                        s_call_active_obj = true;
                        s_state = CallState::LINK_ESTABLISHING;
                        s_timeout_ms = millis() + 30000;
                    } else {
                        Transport::request_path(s_dest_hash);
                        s_state = CallState::PATH_REQUESTING;
                        s_timeout_ms = millis() + 10000;
                    }
                    post_state_event();
                }
            }
        }
    }

    // Path resolution poll (UIM:1602-1619)
    if (s_state == CallState::PATH_REQUESTING && Transport::has_path(s_dest_hash)) {
        Identity peer_identity = Identity::recall(s_peer_hash);
        if (!peer_identity) { call_ended(); return; }
        Destination peer_dest(peer_identity, Type::Destination::OUT,
                              Type::Destination::SINGLE, "lxst", "telephony");
        s_call_link = Link(peer_dest, on_call_link_established,
                           [](Link& l) {
            if (s_state != CallState::IDLE) s_link_closed_pending = true;
        });
        s_call_active_obj = true;
        s_state = CallState::LINK_ESTABLISHING;
        s_timeout_ms = millis() + 30000;
        post_state_event();
    }

    // Timeouts (UIM:1621-1649)
    if (s_timeout_ms > 0 && now > s_timeout_ms) {
        switch (s_state) {
            case CallState::PATH_REQUESTING:
            case CallState::LINK_ESTABLISHING:
            case CallState::WAIT_AVAILABLE:
            case CallState::WAIT_RINGING:
            case CallState::RINGING:
                call_ended();
                return;
            case CallState::INCOMING_RINGING:
                call_ended(true);   // missed call
                return;
            default:
                s_timeout_ms = 0;
                break;
        }
    }

    // Link health + TX pump (UIM:1651-1685)
    if (s_state == CallState::ACTIVE || s_state == CallState::CONNECTING) {
        if (!s_call_active_obj || s_call_link.status() == Type::Link::CLOSED) {
            call_ended();
            return;
        }
        pump_call_tx();
    }
}

// ── setup (UIM:303-315 + LXSTAnnounceHandler UIM:36-42) ────────────────────
class LxstAnnounceHandler : public AnnounceHandler {
public:
    LxstAnnounceHandler() : AnnounceHandler("lxst.telephony") {}
    void received_announce(const Bytes& dest_hash, const Identity& identity,
                           const Bytes& app_data) override {
        // aspect=1 marks a voice-capable peer. The dest_hash here is the
        // peer's lxst.telephony destination; dialing still goes by any
        // hash whose identity is cached (see pyxis_call_initiate).
        pyxis_internal_post_event(PyxisEvent::ANNOUNCE,
                                  dest_hash.toHex().c_str(), "", nullptr,
                                  1);
    }
};
static std::shared_ptr<LxstAnnounceHandler> s_lxst_handler;

bool pyxis_call_setup(Identity& identity) {
    audio_preallocate();
    s_lxst_dest = Destination(identity, Type::Destination::IN,
                              Type::Destination::SINGLE, "lxst", "telephony");
    s_lxst_dest.set_proof_strategy(Type::Destination::PROVE_NONE);
    s_lxst_dest.set_link_established_callback(on_lxst_link_established);
    s_lxst_handler = std::make_shared<LxstAnnounceHandler>();
    Transport::register_announce_handler(HAnnounceHandler(s_lxst_handler));
    Serial.printf("[call] LXST listening on %s\n",
                  s_lxst_dest.hash().toHex().c_str());
    return true;
}

void pyxis_call_announce() {
    if (s_lxst_dest) s_lxst_dest.announce();
}

// ── public control API ─────────────────────────────────────────────────────
bool pyxis_call_initiate(const char* dest_hex) {
    if (!dest_hex || strlen(dest_hex) != 32) return false;
    if (s_state != CallState::IDLE || s_initiate_pending) return false;
    strlcpy(s_initiate_hex, dest_hex, sizeof(s_initiate_hex));
    s_initiate_pending = true;
    return true;
}

bool pyxis_call_answer() {
    if (s_state != CallState::INCOMING_RINGING) return false;
    s_answer_pending = true;
    return true;
}

bool pyxis_call_hangup() {
    if (s_state == CallState::IDLE) return false;
    s_hangup_pending = true;
    return true;
}

bool pyxis_call_set_mute(bool muted) {
    s_muted = muted;
    if (s_capture) s_capture->setMute(muted);
    return true;
}
bool pyxis_call_muted() { return s_muted; }

const char* pyxis_call_state_name() { return state_name(s_state); }

uint32_t pyxis_call_duration_s() {
    return (s_state == CallState::ACTIVE) ? (millis() - s_start_ms) / 1000 : 0;
}

void pyxis_call_peer(char out_hex[33]) {
    out_hex[0] = 0;
    if (s_peer_hash) strlcpy(out_hex, s_peer_hash.toHex().c_str(), 33);
}

void pyxis_call_stats(uint32_t* tx, uint32_t* rx, int* play_buffered,
                      int* cap_avail, uint32_t* underruns) {
    if (tx) *tx = s_tx_count;
    if (rx) *rx = s_rx_count;
    if (play_buffered) *play_buffered = pcm_buffered();
    if (cap_avail) *cap_avail = s_capture ? s_capture->availablePackets() : -1;
    if (underruns) *underruns = s_underruns;
}

bool pyxis_call_set_profile(int profile) {
    if (profile_to_codec2_mode(profile) < 0) return false;
    s_preferred_profile = profile;
    return true;
}
int pyxis_call_get_profile() { return s_preferred_profile; }

#ifdef HYBRID_TEST_HOOKS
void pyxis_call_set_inject_sine(bool enabled, int freq, float amp) {
    if (s_capture) s_capture->setInjectSine(enabled, freq, amp);
}
#endif
