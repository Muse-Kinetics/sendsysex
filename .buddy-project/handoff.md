# Handoff

## What This Project Is

SendSysEx is a C++11 CLI tool built with CMake. It has two modes:
1. **Raw send** — send any `.syx` file to a named/numbered MIDI port
2. **Automatic firmware update** — family-aware two-step bootloader update using JSON device/family databases

## Where things stand (2026-09-06)

**v0.15.0 is shipped and solid on macOS and Windows.** The chunked `--fw-update` reliability work was
validated on real hardware on **both** platforms (Windows over WMS *and* WinMM — the old release
gate, now cleared), and `--bootloader-install` is tested and working on both for SoftStep and 12 Step.
macOS ships a universal notarized `.pkg` installer.

The four firmware-update reliability mechanisms (all tool + `data/families/*.json`, **no MIDI_CPP
change**) are described in full in `decisions.md`:

- CoreMIDI `drain()` pacing in `chunkedSysExTransfer.h` (no-op on Windows).
- `rebootsToAppOnFinalChunk` — skip the doomed final-chunk bootloader handshake.
- `versionEncoding: "bcd16"` — QuNeo's 2-byte nibble-BCD id-reply decode (1.2.31, was misread "18.0.0").
- `confirmByAppReconnectOnly` — reconnect-based confirmation for EM1 firmware (MalletStation) that
  has no standard identity reply (reports version via OSC-over-SysEx only).

**Open front: a safety-critical bug in the bootloader-install dump gate.** A failing
`--bootloader-install softstep` run on Linux was root-caused (2026-09-06) — it is **not** ALSA
gating SysEx. `captureFirmwareDump` listens on *every* MIDI input port and accepts the first
`F0`-prefixed message, so ALSA's `Midi Through` loopback fed the tool **its own request**, which then
**false-passed** validation (`size>10 && F0 … F7 && header`) because the request carries the same KMI
header. The trojan was then flashed to a device whose real image was never read. Any loopback or DIN
out-to-in cable reproduces this on macOS/Windows too. Fixes: restrict listeners to the device's ports,
reject an echo of the request, enforce a minimum image size. See `current-task.md` / `blockers.md`.

Separately, a source-built alsa-lib in `/usr/local` (2026-09-04) shadows the distro one and breaks
every ALSA client on this machine (`aconnect -l` included); `LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu`
is the workaround.

## Branch landscape (check before releasing — "a lot going on")

- **`main`** (current, `34f6031`): the release line — WMS support, legacy trojan tooling, all
  firmware payloads, the reliability fixes above, interactive mode, and the macOS `.pkg` tooling.
  This is where v0.15.0 shipped from.
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

- All CMake builds on macOS, Windows, and Linux
- WinMM and WMS backends (Windows): runtime probe with fallback; `--midi-backend` flag to force
- Firmware update for: 12 Step, BopPad, K-Board, KBP4, MalletStation, MimicHub, QuNeo, QuNexus, SoftStep, SoundStation
- SoftStep + 12 Step legacy bootloader-trojan install (`--bl-send`, `--bootloader-install <family>`) — hardware-validated on **macOS and Windows** (SoftStep v93, 12 Step v28). **Not working on Linux/ALSA** — see `blockers.md`.
- Windows release packaging via `package-release.ps1`

## Key Files to Know

- `src/SendSysEx.cpp` — main argument parsing, mode dispatch
- `src/bootloaderUpgrade.cpp` / `inc/bootloaderUpgrade.h` — firmware update state machine
- `src/deviceDatabase.cpp` — loads and queries `data/kmi_device_database.json` + family JSONs
- `data/families/*.json` — per-family discovery, identity, transport timing
- `inc/rtmidi` — submodule; **do not replace** — this is the KMI WMS fork
- `CMakeLists.txt` — version is the single source of truth

## What needs to happen next

1. **Harden `captureFirmwareDump` + `runLegacyVersionQuery`** — restrict listeners to the device's
   own ports (skip `Midi Through`/loopback), reject a capture identical to the request just sent, and
   enforce a minimum plausible image size. Safety-critical on **all** platforms.
2. **Clean up the `/usr/local` alsa-lib shadow**, then re-run `--bootloader-install softstep` on Linux
   with the fixes and decide whether Linux joins the validated platform list.
3. **Reconcile the `WMS` branch** cmake refactor (`origin/WMS` `56580a7`) into the release line, or
   consciously defer it. See "Branch landscape" above.

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

## Release Process

See `RELEASING.md`. Windows only. Requires the SafeNet code-signing token (office-only).
