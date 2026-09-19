# Current Task

## Status (2026-09-11): post-v0.15.0 maintenance — WinMM RtMidi fixes + 12 Step WinMM port-name fix

Branch `main`. Version **0.15.0** (`CMakeLists.txt`, README in sync). Everything here is unreleased
since the v0.15.0 tag.

### Latest (2026-09-13)

1. **`inc/rtmidi` pin → `cd25104`** (fork `sysex-send-flowcontrol`). Two commits beyond `04155bb`
   that `ef503f8` pinned:
   - `04155bb` — the Windows MIDI Services backend review fixes: the C API gained
     `RTMIDI_API_WINDOWS_MIDI_SERVICES` and `RTMIDI_ERROR_DRIVER_NOT_INSTALLED`; COM is no longer
     initialized on a caller's thread (a Qt GUI thread can still call `OleInitialize()`); input
     delivers only UMP packets with a MIDI 1.0 form on the port's own group; sends split at whole
     packets; `close()` waits for a callback in progress and no longer deadlocks when called from
     one; the endpoint watcher handles updates and is never released under the loader lock.
   - `cd25104` — **WMS port names now identify their device.** A group terminal block's name does
     not have to: when Windows renames a device whose name is taken, the composed block name can
     lose the product entirely. This machine's second 12 Step2 reported `2 - Control Surface`,
     so family matching could not find the port a firmware update needs. The backend now prefixes
     the endpoint name unless the block name already contains it.
2. **`--app-port` no longer hijacks the bootloader stage** (`src/kmiDevice.cpp`). It used to copy the
   app-port name into the bootloader alias, so after bootloader entry the tool waited for a port that
   had just disappeared — observed as a 17-minute hang that left a 12 Step in bootloader mode. With no
   `--bootloader-port`, family bootloader discovery is used instead, and the banner says
   `bootloader=(family discovery)`.
3. **The WMS normalizer strips Windows' `<n> - ` duplicate-name prefix** (`src/deviceDatabase.cpp`),
   which the WinMM and UWP normalizers already did. WMS can carry it in both halves of a block name
   (`2 - 12 Step2 2 - TRS MIDI Out`), so it is removed wherever it appears.
4. **Firmware updates verified over `--midi-backend wms`** on this machine: QuNexus 2.2.1, SoftStep
   2.0.4 then 2.0.8, and 12 Step 1.0.10 — bootloader entry, chunked transfer, reboot, rediscovery and
   version confirmation, all with exit 0. The 12 Step is discovered over WMS without `--app-port`
   since `cd25104`.

**Open:** `lib/MIDI_CPP` (`dec4b78`) pins its nested `examples/rtmidi` at `04155bb`, so it is one
commit behind `inc/rtmidi` again. SendSysEx does not build that copy.

### Earlier (2026-09-11)

1. **`inc/rtmidi` pin `edb5985` → `f90f98e`.** On the fork's `sysex-send-flowcontrol`:
   - `f3d37ae` merges the WinMM fixes from `pr/winmm-send-path`.
   - `f90f98e` follows up so a failed send leaves the SysEx framing state unchanged, so a retried span
     is routed the same way.

   The upstream PR branch carries both as a single commit, `90cb723`. The fork keeps its API
   (`int sendMessage`, `drain()`, the WMS backend), and all of it is pushed to Muse-Kinetics/rtmidi.
   The WinMM backend changes:
   - SysEx is routed by framing state, not length. A final span of 1–3 bytes used to go through
     `midiOutShortMsg()` and never reach the device (thestk/rtmidi #373).
   - The `MIDIHDR` is heap-allocated and freed only after `midiOutUnprepareHeader()` succeeds. The
     unprepare wait is a bounded 5 s deadline (#377).
   - `MidiInWinMM::closePort()` teardown is safe. It used to crash the process (`0xC0000409`) when a
     device rebooted or disappeared with an input port open, e.g. at the end of `--fw-update` (#376).
   - **The WinMM busy-retry loop is removed.** See `decisions.md`.

   Hardware results (Windows 11, `--midi-backend winmm`):
   - **#373 comparison, QuNexus and SoftStep:** single-message images in 492/495-byte windows, so the
     final window is 3 or 2 bytes. Old fork: 3/3 failures (`MMSYSERR_INVALPARAM`, device left in its
     bootloader). Fixed code: passes.
   - **#376:** the QuNexus restore no longer crashes; it exits 0 with the version confirmed.
   - **Fault injection:** a local test build forced seven WinMM failure paths (output prepare/send/
     unprepare errors, a driver that never releases, `midiOutClose` busy, `midiInUnprepareHeader`
     failing on close). All seven were handled without a crash, and the bounded waits measured ~5 s.
   - **Which build ran what:**
     - The firmware-update tests ran on the `pr/winmm-send-path` build.
     - The merged `f3d37ae` build is smoke-tested with identity requests over WMS and WinMM.
     - `f90f98e` (the retry-state follow-up) was build-verified at the time; firmware updates on
       its descendant `cd25104` are hardware-verified over WMS (see 2026-09-13 above).
2. **12 Step not discovered over WinMM — fixed in `src/deviceDatabase.cpp`.**
   - **Cause:** Windows prefixes an already-taken device name with `<n> - ` (`2 - 12 Step2 8`,
     `MIDIOUT2 (2 - 12 Step2) 12`). The WinMM/UWP normalizers kept the prefix, so nothing matched the
     family.
   - **Fix:** they now strip `<digits> - `.
   - **Verified offline:** the four names this machine reported now map to control surface /
     TRS MIDI out / CV out. SoftStep, 12 Step gen 1 and other devices are unchanged.

### Uncommitted in this checkout (12 Step 1.1.0)

- `syx/12Step/12 Step Firmware v1.1.0_cs512.syx` (new, 90220 bytes) and `data/families/12step.json`.
  `firmware_1_1_0` takes default/latest, `firmware_1_0_10` is marked superseded, and
  `identity.applicationPidLsb` becomes `[19, 20, 22]`.
- 1.1.0 is 1.0.10 renumbered - the 12 Step display cannot show a two-digit field - plus one wire
  change: a 12 Step 1 now reports MIDI product ID 19 (0x13) to match the USB product ID it has
  enumerated with since 1.0.9. 20 stays in the list for units still on older firmware. A host
  addressing a unit should send 22, which every version accepts.
- Flashed and verified on a 12 Step 2 (reports 0x16, application 1.1.0, 28/28 host suite).

### Recently landed on `main`

- `de23acc` — SoftStep **2.0.9** is the default/latest payload: each hardware revision reports
  its own USB and MIDI product ID (SS1 = 10, SS2 = 11, SS3 = 13), so an SS1 and an SS2 are
  finally distinguishable on the wire. Verified on a SoftStep 2 (USB 000C -> 000B, MIDI 0x0B,
  26-test suite green). 2.0.8 is marked superseded.
- `e775eab` / `678ab5c` — 12 Step firmware **1.0.10** is the default payload: tether tare fix,
  preset/settings/globals read-back, factory-preset and settings reset commands. `678ab5c` rebuilt
  the image so it refuses `REQUEST_PRESET` during a setlist download. `e775eab` also records the
  firmware-source evidence for the 12 Step CRC/trailer flags.
- `7c35931` — `lib/MIDI_CPP` → `66874eb`, which moves its nested `examples/rtmidi` to `f3d37ae`.
- `746340b` — SoftStep **2.0.8** is the default/latest payload (tether calibration modes stream raw
  ADC again; preset/settings/globals read-back). Verified over WMS on a SoftStep 3.
- `c08c9a8` — per-family `transport.requiresSignedCrc` / `transport.usesLegacyTrailer`, and
  `lib/MIDI_CPP` bumped to `11f5e11` (`setSignedCrc()` / `setTrailerFormat()`). SoftStep is proven
  from captured bytes; 12 Step was verified against its firmware source in `e775eab`. SendSysEx
  itself does not read these flags yet.
- `20b672c` — `.gitmodules` tracks `inc/rtmidi` on `sysex-send-flowcontrol` (was `WMS`).
- `3be3cb9` — `-f` overrides the firmware payload in `--fw-update`. `--fw-version` restores the
  version match.
- `0a7ffab` — EM Pro family added; POLICY-00 added to `guidelines.md`.
- `4eeb27b` — bootloader-install dump-gate safety fixes and the Linux/ALSA send path. The first
  end-to-end Linux bootloader install succeeded on 2026-09-06; see `blockers.md` → Resolved and
  `decisions.md`.

### Priority order (next)

1. **Decide whether Linux joins the supported platform list.** The full path works; it is a
   packaging/support-policy call.
2. **Reconcile the `WMS` branch** (`origin/WMS`, `56580a7`, cmake/rtmidi refactor, not in `main`).
   Merge it or consciously defer. `origin/bootloader_ug` is fully merged into `main`.

### Phase

Post-v0.15.0 — unreleased maintenance on `main` (device database, payloads, RtMidi WinMM fixes).
