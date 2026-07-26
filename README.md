# Gemini is based on a highly-modified fork of MeshPunk, and utilizes Torlando’s [highly modified fork](https://github.com/torlando-tech/microReticulum/tree/feat/t-deck) of [microReticulum](https://github.com/attermann/microReticulum) as a service. 

Gemini is a real mesh phone on ESP32. Voice calls, multi-network text messaging on a standalone device.  Gemini provides T-Deck users a unified mesh experience facilitating voice calls over LXST, and text messaging over either LXMF or MeshCore. 

Reticulum owns the 2.4ghz interface, using either Auto Interface or TCP to manage connectivity. MeshCore owns the LoRa side. One unified address book for both LXMF and MeshCore contacts. 

Because it is based on MeshPunk, any of the Lua apps available in that App Library are available to you here, as well.   Roadmap:
* Add MeshCore Message button to Address Book Contact Card
* Automatic text message retries on both networks
* LXMF file attachments
* Nomadnet Browser
* Better call audio quality


## Project Structure

- `/src` - Main C++ code
  - `main.cpp` - Main application code
- `/data` - Data files that get uploaded to the device filesystem
  - `/lua` - Lua scripts
    - `/apps` - Lua apps
    
## Requirements for Development

- PlatformIO
- T-Deck device
- Git (for submodules)

## Building and Development

0. `softwareupdate --install-rosetta` (macOS on Apple Silicon only — the xtensa toolchain needs Rosetta)
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
   This will upload only the firmware, not the filesystem data.
6. To upload the filesystem data (when changing lua scripts)
   ```
   pio run --target uploadfs
   ```
7. To build release artifacts (written to `releases/`): the release env embeds
   the `data/` tree into the app so the published binaries are self-contained
   ```
   pio run -e meshpunk_release
   ```
   If it reports missing littlefs, run `pio run -t buildfs` first; if the
   firmware was already up to date, force the artifact step with
   `pio run -e meshpunk_release -t mergebin`.

## VSCode hints

You must close the serial monitor before uploadfs or it wont work.

## License

WAS MIT. Now GPL-3/0

## Credits

- Original MeshPunk firmware, which this project is forked from and builds upon:
  - Ben Nolan — https://github.com/bnolan
  - Cameron L — https://github.com/mueslimak3r
- Torlando ([Pyxis](https://github.com/torlando-tech/pyxis/)
- Atterman ([microReticulum](https://github.com/attermann/microReticulum))
- Mark Qvist ([Reticulum](https://github.com/markqvist/Reticulum))
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
Some features have been added or documented with the assistance of AI 
