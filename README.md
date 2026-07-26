# Gemini

is based on a highly-modified fork of MeshPunk, and utilizes
[Torlando's fork of microReticulum](https://github.com/torlando-tech/microReticulum/tree/6054f6ba82367628a85cd07fcb668b95e947f046)
(pinned at the exact commit this firmware builds against) of
[microReticulum](https://github.com/attermann/microReticulum) as a service.

Claude Code did almost all of the heavy lifting.

Gemini is a real mesh phone on ESP32. Voice calls, multi-network text
messaging on a standalone device. Gemini provides T-Deck users a unified
mesh experience facilitating voice calls over LXST, and text messaging
over either LXMF or MeshCore. Calls connect and carry two-way audio
today; quality tuning is on the roadmap.

Reticulum owns the 2.4 GHz WiFi interface, using either AutoInterface or
TCP to manage connectivity (selectable in Settings — TCP-only is a
first-class mode for congested networks). MeshCore owns the LoRa side.
One unified address book for both LXMF and MeshCore contacts.

Because it is based on MeshPunk, any of the Lua apps available in that
App Library are available to you here, as well.

Roadmap:
* Add MeshCore Message button to Address Book Contact Card
* Automatic text message retries on both networks
* LXMF file attachments
* Nomadnet Browser
* Better call audio quality

## Flashing (the simple way — no build environment needed)

Grab the merged image from [Releases](https://github.com/genemichael/Gemini/releases)
and flash it at offset 0 with esptool (`pip install esptool`):

```
python3 -m esptool --chip esp32s3 --port <PORT> --baud 460800 \
    write_flash 0x0 gemini-release.bin
```

- `<PORT>` is `/dev/cu.usbmodemXXX` on macOS, `/dev/ttyACM0` on Linux.
- For a factory-fresh device state, run `... erase_flash` first
  (this wipes any existing identity — see below).
- First boot shows "Unpacking filesystem…" for a few minutes. Do not
  power off. The device then generates its own identities; set WiFi
  and your names in Settings.
- `gemini-test.bin` is the developer build: same firmware plus a `T:`
  serial harness (state probes, call control, identity export/import,
  file push). It exposes the RNS private key over USB by design — use
  it for your own devices, not handouts.
- If flashing fails to connect: hold **BOOT**, tap **RST**, release
  BOOT to force download mode, then retry.

## Project Structure

- `/src` - Main C++ code
  - `main.cpp` - Main application code
- `/lib/pyxis_core` - The embedded Reticulum/LXMF/LXST service
- `/data` - Data files that get uploaded to the device filesystem
  - `/lua` - Lua scripts
    - `/apps` - Lua apps

## Requirements for Development

- PlatformIO
- LilyGo T-Deck (Plus recommended — GPS features assume it)
- Git (for submodules)

## Building and Development

0. `softwareupdate --install-rosetta` (macOS on Apple Silicon only — the
   xtensa toolchain needs Rosetta)
1. Clone this repository
2. Initialize the submodules:
   ```
   git submodule update --init --recursive
   ```
3. Open in PlatformIO
4. Edit Lua scripts in the `/data/lua` directory
5. Build and upload to your T-Deck device:
   ```
   pio run --target upload
   ```
   This uploads only the firmware, not the filesystem data.
6. To upload the filesystem data (when changing lua scripts):
   ```
   pio run --target uploadfs
   ```
   **WARNING: `uploadfs` replaces the entire data partition — your
   MeshCore identity, saved WiFi networks, and preferences are wiped**
   (the RNS identity survives; it lives in NVS). On a `test` build,
   prefer `tools/push_lua.py` to update individual Lua files with no
   wipe. Close the serial monitor first either way or the upload fails.
7. To build release artifacts (written to `releases/`): the release env
   embeds the `data/` tree into the app so the published binaries are
   self-contained
   ```
   pio run -e meshpunk_release
   ```
   If it reports missing littlefs, run `pio run -t buildfs` first; if
   the firmware was already up to date, force the artifact step with
   `pio run -e meshpunk_release -t mergebin`.

## License

This combined work is licensed **GPL-3.0** (see `COPYING` and `NOTICE`).
The original MeshPunk code it builds on remains MIT-licensed; the GPL
applies to the combination, which incorporates GPL-3.0 code from Pyxis.

## Credits

- Original MeshPunk firmware, which this project is forked from and builds upon:
  - Ben Nolan — https://github.com/bnolan
  - Cameron L — https://github.com/mueslimak3r
- MeshCore — https://github.com/ripplebiz/MeshCore (the LoRa mesh side)
- Torlando ([Pyxis](https://github.com/torlando-tech/pyxis/))
- Attermann ([microReticulum](https://github.com/attermann/microReticulum) and [microStore](https://github.com/attermann/microStore))
- Mark Qvist ([Reticulum](https://github.com/markqvist/Reticulum))
- sh123 ([esp32_codec2](https://github.com/sh123/esp32_codec2) — voice codec build)
- LuaVGL by XuNeo: https://github.com/XuNeo/luavgl
- LVGL: https://lvgl.io/
- LilyGo for the T-Deck hardware
- Emojis from https://github.com/googlefonts/noto-emoji
- Emoji converted to .bin with ImageMagick
- doomgeneric https://github.com/ozkl/doomgeneric
   Doom music via Chocolate Doom's OPL/MIDI stack and the DOSBox dbopl emulator
- Pico8 emulation done with fake08 https://github.com/jtothebell/fake-08
   conversion of fake08 to meshpunk elf done by https://github.com/mintylinux
- GameBoy emulation via the gnuboy core from retro-go https://github.com/ducalex/retro-go
- PC-XT emulation via Faux86-remake https://github.com/ArnoldUK/Faux86-remake (lineage: Fake86 by Mike Chambers, Faux86 by James Howard)

## Branch

This branch of the Meshpunk project focuses on extending functionality.
Features have been added or documented with the assistance of AI.
