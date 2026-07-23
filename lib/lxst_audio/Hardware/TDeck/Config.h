// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shim replacing pyxis's lib/tdeck_ui/Hardware/TDeck/Config.h for the
// vendored lxst_audio sources — same namespace shape, ONLY the audio
// constants they reference, so the .cpp files carry zero diffs against
// upstream pyxis. Values cross-checked against this repo's
// src/tdeck-pins.h (TDECK_I2S_* / TDECK_ES7210_*) and pyxis Config.h;
// both trees agree on every pin. Do not add display/keyboard/etc.
// constants here — the rest of tdeck_ui stays un-vendored by design
// (HYBRID_PLAN D3).
#pragma once

#include <cstdint>

namespace Hardware {
namespace TDeck {

namespace I2C {
    constexpr uint8_t  ES7210_ADDR = 0x40;   // mic array ADC (AD0=GND, AD1=GND)
    constexpr uint32_t FREQUENCY = 400000;
}

namespace Audio {
    // Speaker I2S (I2S_NUM_0) — owned by meshpunk's mixer at runtime
    // (D4); i2s_playback.cpp is excluded from the build but its header
    // is still included in places, so the constants stay defined.
    constexpr uint8_t I2S_BCK  = 7;
    constexpr uint8_t I2S_WS   = 5;
    constexpr uint8_t I2S_DOUT = 6;
    // ES7210 microphone array capture (I2S_NUM_1) — exclusive to the
    // call pipeline; meshpunk never touches this port.
    constexpr uint8_t MIC_MCLK = 48;
    constexpr uint8_t MIC_SCK  = 47;
    constexpr uint8_t MIC_LRCK = 21;
    constexpr uint8_t MIC_DIN  = 14;
}

} // namespace TDeck
} // namespace Hardware
