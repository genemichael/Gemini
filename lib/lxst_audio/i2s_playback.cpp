// Copyright (c) 2024 LXST contributors
// SPDX-License-Identifier: MPL-2.0

#include "i2s_playback.h"

#ifdef ARDUINO
#include <driver/i2s.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <Hardware/TDeck/Config.h>
#include "codec_wrapper.h"
#include "packet_ring_buffer.h"
#include <Arduino.h>

using namespace Hardware::TDeck;

static const char* TAG = "LXST:Playback";

// Serial passthrough (lib/pyxis_core/PyxisCall.cpp) — critical failure
// paths log through this in addition to ESP_LOGE, which is compiled out
// at the production CORE_DEBUG_LEVEL=0 (wadamesh lesson,
// docs/hybrid/WADAMESH_BACKPORT_BRIEF.md §3).
extern "C" void pyxis_log(const char* msg);

// HYBRID (D12, ported from wadamesh — its WADAMESH_FORK_BUILD static-stack
// path, i2s_playback.cpp:106-131 in ../wadamesh): the dynamic 8KB
// xTaskCreatePinnedToCore stack was a call-time internal-heap allocation
// D11 forbids ("8KB stack failed silently, largest block 7,668 < 8,192" on
// device). meshpunk has no equivalent build flag to gate this behind, so
// (per D12 §7) it is unconditional here — the stack itself lives in
// PyxisCall.cpp's boot BSS (mirrors i2s_capture.cpp's s_cap_stack) and is
// handed over through this accessor.
extern "C" void* lxst_playback_get_stack();

I2SPlayback::I2SPlayback() = default;

I2SPlayback::~I2SPlayback() {
    stop();
    releaseBuffers();
}

bool I2SPlayback::configureDecoder(Codec2Wrapper* codec) {
    releaseBuffers();

    if (!codec || !codec->isCreated()) {
        pyxis_log("[PLAY] configureDecoder FAILED: invalid codec pointer");
        ESP_LOGE(TAG, "Invalid codec pointer");
        return false;
    }
    codec_ = codec;

    frameSamples_ = codec_->samplesPerFrame();

    // PCM ring buffer in PSRAM
    pcmRing_ = new PacketRingBuffer(PCM_RING_FRAMES, frameSamples_);

    // Decode buffer in PSRAM — sized for batched frames (Columba sends up to 8 sub-frames)
    decodeBufSize_ = frameSamples_ * 16;
    decodeBuf_ = static_cast<int16_t*>(
        heap_caps_malloc(sizeof(int16_t) * decodeBufSize_, MALLOC_CAP_SPIRAM));

    // Drop buffer (for ring overflow discard)
    dropBuf_ = static_cast<int16_t*>(
        heap_caps_malloc(sizeof(int16_t) * frameSamples_, MALLOC_CAP_SPIRAM));

    // PSRAM allocation failures were previously unchecked: a null
    // decodeBuf_ makes writeEncodedPacket() return false forever (dead
    // RX audio with no log); a null ring silently drops every frame.
    // Fail loudly and cleanly instead.
    if (!pcmRing_ || !pcmRing_->isValid() || !decodeBuf_ || !dropBuf_) {
        pyxis_log("[PLAY] configureDecoder FAILED: PSRAM alloc (ring/decode/drop)");
        ESP_LOGE(TAG, "configureDecoder: PSRAM allocation failed");
        releaseBuffers();
        return false;
    }

    ESP_LOGI(TAG, "Decoder configured: Codec2 mode %d, %d samples/frame",
             codec_->libraryMode(), frameSamples_);
    return true;
}

bool I2SPlayback::start() {
    if (!codec_ || playing_.load()) return false;

    // Caller (LXSTAudio) is responsible for calling tone_deinit() first.
    // Defensively uninstall in case it wasn't done.
    i2s_driver_uninstall(I2S_NUM_0);

    // Configure I2S_NUM_0 for voice playback
    i2s_config_t i2s_config = {};
    i2s_config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
    i2s_config.sample_rate = SAMPLE_RATE;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_config.dma_buf_count = 8;
    i2s_config.dma_buf_len = 64;
    i2s_config.use_apll = false;
    i2s_config.tx_desc_auto_clear = true;
    i2s_config.fixed_mclk = 0;

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        char logbuf[64];
        snprintf(logbuf, sizeof(logbuf), "[PLAY] I2S_NUM_0 driver install FAILED: %d", (int)err);
        pyxis_log(logbuf);
        ESP_LOGE(TAG, "I2S_NUM_0 driver install failed: %d", err);
        return false;
    }

    i2s_pin_config_t pin_config = {};
    pin_config.mck_io_num = I2S_PIN_NO_CHANGE;
    pin_config.bck_io_num = Audio::I2S_BCK;
    pin_config.ws_io_num = Audio::I2S_WS;
    pin_config.data_out_num = Audio::I2S_DOUT;
    pin_config.data_in_num = I2S_PIN_NO_CHANGE;

    err = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (err != ESP_OK) {
        char logbuf[64];
        snprintf(logbuf, sizeof(logbuf), "[PLAY] I2S_NUM_0 pin config FAILED: %d", (int)err);
        pyxis_log(logbuf);
        ESP_LOGE(TAG, "I2S_NUM_0 pin config failed: %d", err);
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }

    i2sInitialized_ = true;

    // Reset ring and prebuffer state
    if (pcmRing_) pcmRing_->reset();

    playing_.store(true, std::memory_order_relaxed);
    taskExited_.store(false, std::memory_order_relaxed);   // re-armed for this run

    // HYBRID (D12): static stack, boot-reserved in PyxisCall.cpp's BSS —
    // no call-time internal-heap allocation (D11). The TCB is a function-
    // static here (one I2SPlayback instance ever exists, in PyxisCall.cpp)
    // so it, too, costs nothing at call time.
    static StaticTask_t s_play_tcb;
    StackType_t* stack = (StackType_t*)lxst_playback_get_stack();
    if (!stack) {
        // Boot reservation missing/failed — this exact failure (dynamic
        // stack alloc at call time) cost wadamesh two debugging rounds;
        // ESP_LOGE alone is invisible at CORE_DEBUG_LEVEL=0.
        pyxis_log("[PLAY] no playback stack (boot reservation missing)");
        ESP_LOGE(TAG, "Failed to create playback task: no static stack");
        playing_.store(false, std::memory_order_relaxed);
        i2s_driver_uninstall(I2S_NUM_0);
        i2sInitialized_ = false;
        return false;
    }
    taskHandle_ = xTaskCreateStaticPinnedToCore(
        playbackTask, "lxst_play", PLAYBACK_TASK_STACK / sizeof(StackType_t),
        this, PLAYBACK_TASK_PRIORITY, stack, &s_play_tcb, PLAYBACK_TASK_CORE);

    if (taskHandle_ == nullptr) {
        pyxis_log("[PLAY] playback task create FAILED (static)");
        ESP_LOGE(TAG, "Failed to create playback task");
        playing_.store(false, std::memory_order_relaxed);
        i2s_driver_uninstall(I2S_NUM_0);
        i2sInitialized_ = false;
        return false;
    }

    ESP_LOGI(TAG, "Playback started");
    return true;
}

void I2SPlayback::stop() {
    if (!playing_.load()) return;

    playing_.store(false, std::memory_order_relaxed);

    // HYBRID (D12 hardening — wadamesh LACKS this, deliberate addition):
    // the reference stop() did a blind vTaskDelay(50ms) then nulled
    // taskHandle_ and uninstalled I2S0. If playbackLoop() was mid-i2s_write
    // (100ms timeout, see below) it could still be running past 50ms —
    // either we uninstall I2S0 under a live writer, or the task runs on
    // after the caller has already reinstalled the mixer and writes into
    // ITS driver. Doubly required here because the task stack is a single
    // static BSS block reused across calls (start() must not hand it to a
    // new task until this one is fully past its last I2S touch).
    if (taskHandle_) {
        uint32_t waited = 0;
        while (!taskExited_.load(std::memory_order_acquire) && waited < 300) {
            vTaskDelay(pdMS_TO_TICKS(5));
            waited += 5;
        }
        if (!taskExited_.load(std::memory_order_acquire)) {
            pyxis_log("[PLAY] ERROR: playback task did not exit in 300ms — teardown proceeding UNSAFELY");
            ESP_LOGE(TAG, "playback task did not exit in 300ms");
        }
        taskHandle_ = nullptr;
    }

    if (i2sInitialized_) {
        // Write silence to flush DMA
        int16_t silence[128] = {0};
        size_t written;
        i2s_write(I2S_NUM_0, silence, sizeof(silence), &written, pdMS_TO_TICKS(100));

        i2s_stop(I2S_NUM_0);
        i2s_driver_uninstall(I2S_NUM_0);
        i2sInitialized_ = false;
    }

    // Re-init Tone.cpp's I2S driver so notification tones work again
    // The next call to tone_play() will re-initialize via tone_init()

    ESP_LOGI(TAG, "Playback stopped");
}

void I2SPlayback::releaseBuffers() {
    codec_ = nullptr;  // Not owned — don't delete
    delete pcmRing_;
    pcmRing_ = nullptr;
    free(decodeBuf_);
    decodeBuf_ = nullptr;
    free(dropBuf_);
    dropBuf_ = nullptr;
    decodeBufSize_ = 0;
    frameSamples_ = 0;
}

bool I2SPlayback::writeEncodedPacket(const uint8_t* data, int length) {
    if (!codec_ || !pcmRing_ || !decodeBuf_ || !frameSamples_) return false;

    int decodedSamples = codec_->decode(data, length, decodeBuf_, decodeBufSize_);
    if (decodedSamples <= 0) {
        Serial.printf("[PLAY] Decode FAIL: in=%d buf=%d\n", length, decodeBufSize_);
        decodeFailCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    decodeOkCount_.fetch_add(1, std::memory_order_relaxed);

    // Tally PCM energy for RMS-based QoS (LXST harness gates on this).
    // Sum-of-squares uses uint64 so int16² ≤ 2³⁰ adds for ~2³⁴ frames
    // before overflow — far longer than any test call.
    uint64_t sumsq = 0;
    for (int s = 0; s < decodedSamples; ++s) {
        int32_t v = decodeBuf_[s];
        sumsq += (uint64_t)(v * v);
    }
    pcmSampleCount_.fetch_add((uint32_t)decodedSamples, std::memory_order_relaxed);
    pcmSumSquares_.fetch_add(sumsq, std::memory_order_relaxed);

    // Write decoded PCM to ring buffer one frame at a time
    // (ring buffer only accepts exactly frameSamples_ per write)
    int numFrames = decodedSamples / frameSamples_;
    for (int i = 0; i < numFrames; i++) {
        int16_t* framePtr = decodeBuf_ + i * frameSamples_;
        if (!pcmRing_->write(framePtr, frameSamples_)) {
            // Ring full — drop this frame (playback task will drain)
            // NOTE: Do NOT call read() here — this is SPSC and
            // the playback task is the sole consumer on another core.
            break;
        }
    }

    return true;
}

int I2SPlayback::bufferedFrames() const {
    if (!pcmRing_) return 0;
    return pcmRing_->availableFrames();
}

void I2SPlayback::playbackTask(void* param) {
    auto* self = static_cast<I2SPlayback*>(param);
    self->playbackLoop();
    // D12: signal stop()'s join BEFORE deleting — the static stack this
    // task runs on must not be handed to a new task until this store is
    // visible and the task has actually left the scheduler.
    self->taskExited_.store(true, std::memory_order_release);
    vTaskDelete(NULL);
}

void I2SPlayback::playbackLoop() {
    ESP_LOGI(TAG, "Playback task running on core %d", xPortGetCoreID());

    // Wait for prebuffer
    bool prebuffered = false;

    // Frame buffer for reading from ring
    int16_t* frameBuf = static_cast<int16_t*>(
        heap_caps_malloc(sizeof(int16_t) * frameSamples_, MALLOC_CAP_SPIRAM));

    // Silence frame for underruns
    int16_t* silenceFrame = static_cast<int16_t*>(
        heap_caps_calloc(frameSamples_, sizeof(int16_t), MALLOC_CAP_SPIRAM));

    // Either alloc failing means the task exits immediately — playback
    // looks "started" to the caller but produces no audio, so say so on
    // production serial. (silenceFrame was previously unchecked and is
    // handed straight to i2s_write on the first underrun.)
    if (!frameBuf || !silenceFrame) {
        pyxis_log("[PLAY] playback task FAILED: PSRAM frame/silence alloc — no RX audio");
        ESP_LOGE(TAG, "Failed to allocate frame buffer");
        free(frameBuf);
        free(silenceFrame);
        return;
    }

    uint32_t framesPlayed = 0;

    while (playing_.load(std::memory_order_relaxed)) {
        // Prebuffer: wait until we have enough frames before starting playback
        if (!prebuffered) {
            if (pcmRing_ && pcmRing_->availableFrames() >= PREBUFFER_FRAMES) {
                prebuffered = true;
                Serial.printf("[PLAY] Prebuffer complete (%d frames)\n",
                              pcmRing_->availableFrames());
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }

        // Read a frame from the ring buffer
        bool hasFrame = pcmRing_ && pcmRing_->read(frameBuf, frameSamples_);
        if (hasFrame) {
            framesPlayed++;
            if (framesPlayed <= 3 || (framesPlayed % 500 == 0)) {
                Serial.printf("[PLAY] Frame #%lu (buf=%d)\n",
                              (unsigned long)framesPlayed, pcmRing_->availableFrames());
            }
        }

        int16_t* outputData;
        if (!hasFrame) {
            // Underrun — output silence
            outputData = silenceFrame;
        } else if (muted_.load(std::memory_order_relaxed)) {
            // Muted — output silence but keep consuming
            outputData = silenceFrame;
        } else {
            outputData = frameBuf;
        }

        // Write to I2S DMA
        size_t bytesWritten;
        esp_err_t err = i2s_write(I2S_NUM_0, outputData,
                                  frameSamples_ * sizeof(int16_t),
                                  &bytesWritten, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S write error: %d", err);
        }
    }

    free(frameBuf);
    free(silenceFrame);

    ESP_LOGI(TAG, "Playback task exiting");
}

#endif // ARDUINO
