# Blockers

## Code-signing certificate is office-only

Windows releases must be signed with the KMI SafeNet token, which is physically at the office.
Remote sessions cannot produce signed releases. **Unblocked by:** being at the office or arranging
remote signing access.

## No automated hardware-in-the-loop tests

Manual smoke-test on real devices is the only validation path. Adding CI would require hardware
fixtures. **Needs:** hardware test rig decision from the team.

---

## Fixed 2026-09-11 (RtMidi fork `f3d37ae`, `src/deviceDatabase.cpp`)

- **WinMM: process crash when a device reboots or disappears with an input port open.**
  - **Symptom:** `MidiInWinMM::closePort()` left its lock held and `connected_` set, then the
    destructor freed the same SysEx buffers a second time. The process died with `0xC0000409`.
  - **When:** at the end of `--fw-update` on the QuNexus, and once after a plain first-send failure.
  - **Fix:** in the RtMidi fork, `inc/rtmidi` → `f3d37ae`. See `current-task.md`.
- **WinMM: SysEx final span of 1–3 bytes never reached the device.** A firmware image sent in
  windows whose last window was that short left the device in its bootloader (`MMSYSERR_INVALPARAM`
  on every attempt). Fixed in the same RtMidi update.
- **12 Step not discovered over WinMM.** Windows' `<n> - ` duplicate-name prefix defeated port-name
  normalization. Fixed in `src/deviceDatabase.cpp`; the hardware check is still pending.

## Resolved

- **`captureFirmwareDump` accepted its own echoed request as a firmware image** — RESOLVED, committed
  in `4eeb27b` (2026-09-06).
  - **Cause:** a loopback (ALSA `Midi Through`, virtual port, DIN out-to-in cable) fed the tool its
    own 75-byte dump request, which passed validation because it carries the same KMI header. It is
    the safety gate before a trojan install.
  - **Fixed by:** listening only on the device's own input ports; rejecting a capture equal to the
    request; requiring ≥ 4096 bytes.
  - **Hardware-verified:** the version query reads v93, and the dump gate captures 66,312 bytes.
- **Linux/ALSA: `--bootloader-install softstep` failed** — RESOLVED 2026-09-06 (`4eeb27b`, plus the
  RtMidi fork's `edb5985`). Three faults:
  1. The echo bug above.
  2. `MidiOutAlsa::sendMessage()` rejected partial SysEx spans. Fixed inside the fork, with no API
     change; see `decisions.md`.
  3. A source-built alsa-lib 1.2.14 with an empty plugin dir. That's environmental, fixed by
     `scripts/fix-alsa-plugin-dir.sh`.

  End-to-end on Linux: SoftStep v93 → trojan (100/100 sectors) → `VERIFY OK`, bootloader 1.0.0 →
  `--fw-update` (290/290 chunks) → application 2.0.7 confirmed.
- **Windows validation of the fw-update reliability fixes** — RESOLVED. Re-run on real hardware over
  both WMS and WinMM; no regressions. This was the v0.15.0 release gate.
- **macOS release packaging** — RESOLVED (2026-08-27). `package-release-macos.sh` builds a universal
  signed/notarized/stapled `.pkg` installer; documented in `RELEASING.md` → macOS.
- **Stale README version header** — RESOLVED. README and `CMakeLists.txt` both read 0.15.0.
