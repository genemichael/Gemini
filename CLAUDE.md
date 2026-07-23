# T-Deck Hybrid Phone — engineering guide for Claude

This branch (`hybrid-phone`) turns meshpunk (LVGL/luavgl launcher +
MeshCore LoRa texting for the LilyGO T-Deck Plus) into a handheld
communicator by embedding the networking/voice core of
`../pyxis` (LXMF messaging + LXST voice over Reticulum/WiFi,
GPL-3.0). The device is a phone, not a repeater/bridge — MeshCore↔RNS
bridging is out of scope.

Read `docs/hybrid/HYBRID_PLAN.md` (settled decisions) and
`docs/hybrid/FINDINGS.md` (code-verified maps of both codebases) before
touching code. `../pyxis` is checked out as reference; copy its solved
problems, cite the source file.

## Hard rules

1. **The SX1262 belongs exclusively to MeshCore.** microReticulum is
   built with NO radio interface: `sx1262_interface` and `ble_interface`
   are excluded entirely. The device is a fully standalone RNS node:
   AutoInterface (peer-to-peer over local WiFi) is the primary interface
   and the device must be fully functional with it alone — never a
   dependency on rnsd or any remote daemon. TCPClientInterface is an
   optional reach-beyond-the-LAN extra: NVS-gated, disabled by default,
   not constructed when no host is configured. MeshCore never touches
   the RNS socket; RNS never touches the radio.
2. **Lua is UI-only.** The Pyxis core runs as a native C++ service.
   Call audio is a native FreeRTOS task per HYBRID_PLAN; audio data never
   crosses the Lua VM. Lua sees only control-plane bindings (`rns.*`,
   `phone.*`) built on meshpunk's existing binding pattern.
3. **The two stacks share nothing but the CPU** (and the arbitrated I2S
   device). Separate NVS namespaces, separate identity/key storage,
   separate on-flash directories.
4. **Verification gate before claiming done:** `pio run -e meshpunk`
   must pass. No hardware attached means you say "compiles, untested on
   device" — never claim runtime behavior you didn't observe. Each
   milestone produces a flashable image + a "what to check on device"
   list in `docs/hybrid/TESTLOG.md`.
5. **Do not casually change the pinned deps or ported build flags.**
   The microReticulum/microLXMF/microStore SHAs and the
   `__EMBEDDED__`/`MEMORY_CRITICAL`/`RNS_*`/`USTORE_*` flag group are
   ported from `../pyxis/platformio.ini`, whose comment blocks document
   the bug each one fixes (duplicate-lib linkage, PSRAM contention,
   path-store failures). Read those comments before touching any of it;
   changes need architect sign-off.
6. **Stock meshpunk behavior must keep working** — launcher, MeshCore
   texting, existing apps. Hybrid code goes in new modules; guard any
   shared-file change so the diff against upstream stays reviewable.
7. **Licensing:** the combined work ships GPL-3.0 (pyxis is GPL-3.0;
   meshpunk/MeshCore are MIT, compatible one-way). Keep license headers
   and attribution intact in every ported file.
8. **Decisions not covered by HYBRID_PLAN.md get escalated**, not
   invented. Write the question + your recommendation in your report and
   stop that sub-task; continue parallel work that isn't blocked.

## Verify

- Compile gate: rule 4 (`pio` is installed; else `python3 -m platformio`).
- Full flashable = firmware + LittleFS data image
  (`pio run -e meshpunk -t buildfs`); `merge_bin.py` combines them.
- On-device testing happens only when Gene flashes hardware. Milestone 2
  needs a desktop rnsd host on the LAN (ask Gene); milestone 3 needs a
  desktop LXST peer.

## Subagent routing (cost-tiered)

Definitions in `.claude/agents/`. Route by risk:

- **phone-architect (Opus):** platformio.ini/partition/pin changes,
  microReticulum interface contracts, task/core layout, I2S ownership
  design, PSRAM budget, anything in rule 5's no-touch list, escalations.
- **phone-builder (Sonnet):** service glue, Lua bindings following the
  documented pattern, Lua apps, ported modules where FINDINGS.md names
  the pattern + source files. Prompts must name pattern file(s), the
  pyxis source to copy from, and the compile gate.
- **phone-scribe (Haiku):** docs, TESTLOG/changelog upkeep, transcribing
  code maps into FINDINGS.md tables, Lua app boilerplate from a spec.

Every delegated prompt includes: rules 1–4, the file list it may touch,
the reference files to copy from, and the compile gate command.
