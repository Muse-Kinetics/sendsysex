# Handoff

## What This Project Is

SendSysEx is a C++11 CLI tool built with CMake. It has two modes:
1. **Raw send** — send any `.syx` file to a named/numbered MIDI port
2. **Automatic firmware update** — family-aware two-step bootloader update using JSON device/family databases

It also carries the legacy bootloader-trojan install for pre-bootloader SoftStep / 12 Step units
(`--bootloader-install`, `--bl-send`), and an interactive mode when run with no arguments.

## Where things stand (2026-09-11)

**v0.15.0 is the current release** (macOS universal notarized `.pkg`, Windows signed zip). `main` has
moved on since the tag (unreleased):

- **Device database and payloads:** EM Pro family; SoftStep **2.0.9** and 12 Step **1.1.0**
  defaults; per-family `requiresSignedCrc` / `usesLegacyTrailer` transport flags (SoftStep proven
  from captured bytes, 12 Step verified against its firmware source); `-f` payload override for
  `--fw-update`.
- **Bootloader-install safety and Linux:** the dump gate no longer accepts its own echoed request.
  The Linux/ALSA send path works: the first end-to-end Linux bootloader install succeeded on
  2026-09-06.
- **RtMidi and port names:**
  - `inc/rtmidi` → `cd25104` (2026-09-13): the WinMM send/close fixes with busy-retry removed, the
    Windows MIDI Services backend review fixes, and WMS port names that identify their device
  - the 12 Step port-name fix for WinMM/UWP, and the same `<n> - ` prefix stripping for WMS
  - `--app-port` without `--bootloader-port` no longer hangs the bootloader stage
  - firmware updates hardware-verified over `--midi-backend wms` on QuNexus, SoftStep and 12 Step

  See `current-task.md`.

The four firmware-update reliability mechanisms (all tool + `data/families/*.json`, **no MIDI_CPP
change**) are described in full in `decisions.md`:

- CoreMIDI `drain()` pacing in `chunkedSysExTransfer.h` (no-op on Windows).
- `rebootsToAppOnFinalChunk` — skip the doomed final-chunk bootloader handshake.
- `versionEncoding: "bcd16"` — QuNeo's 2-byte nibble-BCD id-reply decode (1.2.31, was misread "18.0.0").
- `confirmByAppReconnectOnly` — reconnect-based confirmation for EM1 firmware (MalletStation) that
  has no standard identity reply (reports version via OSC-over-SysEx only).

## Branch landscape

- **`main`**: the release line. v0.15.0 shipped from here.
- **`origin/WMS`** (`56580a7`): the original WMS/RtMidi + cmake-preset branch. It ends with a
  refactor, *"use rtmidi add_subdirectory; drop redundant WMS wiring"*, that is **not in `main`**.
  Diverged both ways — reconcile intentionally or consciously defer; don't fast-forward blindly.
- **`origin/bootloader_ug`** (`214c3c1`): fully merged into `main`; nothing left to reconcile.

**Submodules:**
- `inc/rtmidi` is the Muse-Kinetics RtMidi fork, tracking branch `sysex-send-flowcontrol`. That
  branch is WMS plus CoreMIDI flow-controlled SysEx and `drain()`, `int sendMessage`, ALSA partial
  SysEx, and (`f3d37ae`, `f90f98e`) the WinMM fixes.
- `lib/MIDI_CPP` is at `dec4b78`, whose own nested `examples/rtmidi` pins `04155bb` — one commit
  behind `inc/rtmidi` (`cd25104`). SendSysEx does not build that copy.

## Repository Layout

```
src/          — C++ sources (SendSysEx.cpp is the entry point)
inc/          — Headers; inc/rtmidi is a git submodule (Muse-Kinetics RtMidi fork)
lib/MIDI_CPP  — git submodule; SysEx framing/CRC + device metadata (sources compiled into the binary)
lib/json      — git submodule; nlohmann/json (header-only)
lib/syxMfg    — git submodule; manufacturer-ID lookup
data/         — Device database (kmi_device_database.json), per-family JSONs, schema
syx/          — Firmware payload .syx files, organized by family
scripts/      — helper scripts (e.g. fix-alsa-plugin-dir.sh)
build/        — CMake build output (gitignored)
dist/         — Release zips and checksums (gitignored)
```

## What's Working

- CMake builds on macOS, Windows, and Linux.
- **Windows backends:** WinMM and WMS, with a runtime probe and fallback. `--midi-backend` or
  `KMI_MIDI_BACKEND=winmm` forces one.
- **Firmware update** for: 12 Step, BopPad, EM Pro, K-Board, KBP4, MalletStation, MimicHub, QuNeo,
  QuNexus, SoftStep, SoundStation.
- **Legacy bootloader-trojan install** for SoftStep and 12 Step (`--bl-send`,
  `--bootloader-install <family>`). Hardware-validated on macOS and Windows (SoftStep v93, 12 Step
  v28). SoftStep is also validated end-to-end on Linux/ALSA (2026-09-06). On Windows,
  `--bl-send` requires WMS.
- **Release packaging:** `package-release.ps1` (Windows) and `package-release-macos.sh` (macOS `.pkg`).

## Key Files to Know

- `src/SendSysEx.cpp` — argument parsing, mode dispatch, raw send, legacy bootloader-install flow
- `src/kmiDevice.cpp` / `inc/kmiDevice.h` — port discovery and state, identity handling,
  `runAutomaticUpdate()` (the `--fw-update` state machine)
- `inc/chunkedSysExTransfer.h` — chunked send, per-chunk identity-reply handshake, whole-transfer retry
- `src/bootloaderUpgrade.cpp` / `inc/bootloaderUpgrade.h` — legacy trojan image decode
- `inc/bootloaderSend.h` — legacy sector-wise send (one SysEx message, timed spans)
- `src/deviceDatabase.cpp` — loads `data/kmi_device_database.json` + family JSONs; per-backend
  port-name normalization
- `src/midiBackend.cpp` — Windows WMS/WinMM backend selection
- `data/families/*.json` — per-family discovery, identity, transport timing and wire conventions
- `inc/rtmidi` — submodule; **do not replace** with upstream RtMidi
- `CMakeLists.txt` — version is the single source of truth

## What needs to happen next

See `current-task.md` → Priority order. In short:
1. Decide on Linux support.
2. Reconcile `origin/WMS`.

### Release mechanics (when there is a next release)
- Bump `project(SendSysEx VERSION X.Y.Z)` in `CMakeLists.txt` — the single source of truth — and sync
  the `Version` line + "Last updated" date in `README.md`.
- Package + sign on Windows with `package-release.ps1` (SafeNet token is office-only, see
  `blockers.md`); macOS via `package-release-macos.sh`. Then tag `vX.Y.Z` and `gh release create`
  (see `commands.md`).

### Standing items
- No automated tests — all validation is manual smoke-test on hardware.

## How to Start

```sh
git submodule update --init --recursive
cmake -B build
cmake --build build
./build/SendSysEx -l          # list ports
./build/SendSysEx --help
```

On Windows use a CMake new enough for the `Visual Studio 17 2022` generator (~3.21+). The copy
bundled with VS 2022 works. Building with WMS needs the Windows MIDI Services SDK.

## Release Process

See `RELEASING.md` (Windows and macOS). Windows signing requires the SafeNet code-signing token
(office-only).
