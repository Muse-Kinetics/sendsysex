# Current Task

## Status (2026-09-11): post-v0.15.0 maintenance — WinMM RtMidi fixes + 12 Step WinMM port-name fix

Branch `main`. Version **0.15.0** (`CMakeLists.txt`, README in sync). Everything here is unreleased
since the v0.15.0 tag.

### Latest (2026-09-11)

1. **`inc/rtmidi` pin `edb5985` → `f3d37ae`.** `f3d37ae` merges `pr/winmm-send-path` (`2a6976b`) into
   the fork's `sysex-send-flowcontrol`. It keeps this branch's API: `int sendMessage`, `drain()`, and
   the WMS backend, and is pushed to Muse-Kinetics/rtmidi. The WinMM backend changes:
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
   - **Which build ran what:** the firmware-update tests ran on the `pr/winmm-send-path` build. The
     merged `f3d37ae` build is smoke-tested with identity requests over WMS and WinMM.

   Note: `lib/MIDI_CPP`'s nested `examples/rtmidi` still pins `edb5985`. SendSysEx doesn't build it.
2. **12 Step not discovered over WinMM — fixed in `src/deviceDatabase.cpp`.**
   - **Cause:** Windows prefixes an already-taken device name with `<n> - ` (`2 - 12 Step2 8`,
     `MIDIOUT2 (2 - 12 Step2) 12`). The WinMM/UWP normalizers kept the prefix, so nothing matched the
     family.
   - **Fix:** they now strip `<digits> - `.
   - **Verified offline:** the four names this machine reported now map to control surface /
     TRS MIDI out / CV out. SoftStep, 12 Step gen 1 and other devices are unchanged.

### Recently landed on `main`

- `746340b` — SoftStep **2.0.8** is the default/latest payload (tether calibration modes stream raw
  ADC again; preset/settings/globals read-back). Verified over WMS on a SoftStep 3.
- `c08c9a8` — per-family `transport.requiresSignedCrc` / `transport.usesLegacyTrailer`, and
  `lib/MIDI_CPP` bumped to `11f5e11` (`setSignedCrc()` / `setTrailerFormat()`). SoftStep is proven
  from captured bytes. **12 Step is set on the firmware owner's statement and not yet verified.**
  SendSysEx itself does not read these flags yet.
- `20b672c` — `.gitmodules` tracks `inc/rtmidi` on `sysex-send-flowcontrol` (was `WMS`).
- `3be3cb9` — `-f` overrides the firmware payload in `--fw-update`. `--fw-version` restores the
  version match.
- `0a7ffab` — EM Pro family added; POLICY-00 added to `guidelines.md`.
- `4eeb27b` — bootloader-install dump-gate safety fixes and the Linux/ALSA send path. The first
  end-to-end Linux bootloader install succeeded on 2026-09-06; see `blockers.md` → Resolved and
  `decisions.md`.

### Priority order (next)

1. **Verify 12 Step `requiresSignedCrc` / `usesLegacyTrailer`** against 12 Step firmware or hardware.
2. **Decide whether Linux joins the supported platform list.** The full path works; it is a
   packaging/support-policy call.
3. **Reconcile the `WMS` branch** (`origin/WMS`, `56580a7`, cmake/rtmidi refactor, not in `main`).
   Merge it or consciously defer. `origin/bootloader_ug` is fully merged into `main`.

### Phase

Post-v0.15.0 — unreleased maintenance on `main` (device database, payloads, RtMidi WinMM fixes).
