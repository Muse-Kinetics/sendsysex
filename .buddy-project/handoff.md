# Handoff

## What This Project Is

SendSysEx is a C++11 CLI tool built with CMake. It has two modes:
1. **Raw send** — send any `.syx` file to a named/numbered MIDI port
2. **Automatic firmware update** — family-aware two-step bootloader update using JSON device/family databases

## Latest work (2026-08-25) — chunked `--fw-update` reliability, hardware-validated on macOS

On `bootloader_ug`. Root-caused a class of `--fw-update` failures exposed when the Windows work
(`840f09d`) was exercised on macOS/CoreMIDI, and validated a clean single-pass update on real
hardware for **every** attached family (SoftStep, BopPad, K-Board, QuNexus, 12 Step, QuNeo,
MalletStation; KBP4 unchanged/confirmed). Four mechanisms, all in the tool + `data/families/*.json`,
**no MIDI_CPP change** — see `decisions.md` for the full rationale:

- CoreMIDI `drain()` pacing in `chunkedSysExTransfer.h` (no-op on Windows).
- `rebootsToAppOnFinalChunk` — skip the doomed final-chunk bootloader handshake.
- `versionEncoding: "bcd16"` — QuNeo's 2-byte nibble-BCD id-reply decode (1.2.31, was mis-read "18.0.0").
- `confirmByAppReconnectOnly` — reconnect-based confirmation for EM1 firmware (MalletStation) that
  has no standard identity reply (reports version via OSC-over-SysEx only).

Also split `--help` into `--help` + `--help-bootloader`. New JSON fields are documented in
`data/schemas/kmi_family.schema.json`. **Not yet validated on Windows** (see below).

## Branch landscape (check before releasing — "a lot going on")

- **`bootloader_ug`** (current, `840f09d` + this session's commit): the release-candidate line —
  WMS support, legacy trojan tooling, all firmware payloads, and the reliability fixes above.
- **`main`** (`306f5e6`): behind `bootloader_ug`.
- **`origin/WMS`** (`56580a7`): the foundational WMS/RtMidi + cmake-preset + macOS-build branch,
  with a later refactor *"use rtmidi add_subdirectory; drop redundant WMS wiring"* that is **NOT in
  `bootloader_ug`**. Decide whether that cmake refactor merges into the release line before tagging.
  Diverged both ways — reconcile intentionally, don't fast-forward blindly.

## Repository Layout

```
src/          — C++ sources (SendSysEx.cpp is the entry point)
inc/          — Headers; inc/rtmidi is a git submodule (KMI WMS fork)
lib/MIDI_CPP  — git submodule
data/         — Device database (kmi_device_database.json) and per-family JSONs
syx/          — Firmware payload .syx files, organized by family
build/        — CMake build output (gitignored)
dist/         — Release zips and checksums (gitignored)
```

## What's Working

- All CMake builds on macOS and Windows
- WinMM and WMS backends (Windows): runtime probe with fallback; `--midi-backend` flag to force
- Firmware update for: 12 Step, BopPad, K-Board, KBP4, MalletStation, MimicHub, QuNeo, QuNexus, SoftStep, SoundStation
- SoftStep legacy bootloader-trojan workflow (`--bl-send`, `--bootloader-install`)
- Windows release packaging via `package-release.ps1`

## Key Files to Know

- `src/SendSysEx.cpp` — main argument parsing, mode dispatch
- `src/bootloaderUpgrade.cpp` / `inc/bootloaderUpgrade.h` — firmware update state machine
- `src/deviceDatabase.cpp` — loads and queries `data/kmi_device_database.json` + family JSONs
- `data/families/*.json` — per-family discovery, identity, transport timing
- `inc/rtmidi` — submodule; **do not replace** — this is the KMI WMS fork
- `CMakeLists.txt` — version is the single source of truth

## Before the next release (what needs to happen next)

This session's reliability work is committed on `bootloader_ug` but **not yet released**. Do these,
roughly in order, before tagging:

1. **Validate on Windows.** Re-run `--fw-update <family>` on WMS and WinMM for the families touched
   this session (SoftStep, BopPad, K-Board, QuNexus, 12 Step, QuNeo, MalletStation) plus KBP4.
   The new JSON flags are cross-platform; the `drain()` pacing is a Windows no-op. This is the gate.
2. **Reconcile the `WMS` branch** cmake refactor (`origin/WMS` `56580a7`) into the release line, or
   consciously defer it. See "Branch landscape" above.
3. **Bump the version** — `project(SendSysEx VERSION X.Y.Z)` in `CMakeLists.txt` is the single source
   of truth (currently `0.13.0`; this session's new behavior warrants a bump). Then **sync
   `README.md`** — its `Version` line still reads `0.11.0` (already stale vs 0.13.0) and its "Last
   updated" date.
4. **Changelog / release notes** — capture the four reliability mechanisms and the per-family flags.
5. **Package + sign on Windows** — `package-release.ps1` (Windows only; SafeNet token is office-only,
   see blockers.md), then tag `vX.Y.Z` and `gh release create` (see commands.md).

### Standing items (not release-blocking)
- macOS release packaging is undocumented (no `package-release.ps1` equivalent; Apple notarization
  is a separate credential chain).
- No automated tests — all validation is manual smoke-test on hardware.
- EM Pro Riser shares MalletStation's EM1 firmware → will need `confirmByAppReconnectOnly` when it
  gets a `--fw-update` family entry.

## How to Start

```sh
git submodule update --init --recursive
cmake -B build
cmake --build build
./build/SendSysEx -l          # list ports
./build/SendSysEx --help
```

## Release Process

See `RELEASING.md`. Windows only. Requires the SafeNet code-signing token (office-only).
