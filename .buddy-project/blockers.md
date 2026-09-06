# Blockers

## `captureFirmwareDump` accepts its own echoed request as a valid firmware image

**Status: FIXED 2026-09-06, hardware-verified, uncommitted (awaiting approval).** Root cause and the
three fixes are recorded below and in `current-task.md`.

`captureFirmwareDump` (`src/SendSysEx.cpp:1656`) opens **every** MIDI input port on the system and
accepts the first message from any of them that starts with `F0` and exceeds 4 bytes. If any loopback
exists — ALSA's `Midi Through` port on Linux, a virtual port, or a DIN out-to-in cable on any OS —
the tool receives **its own dump request** back and treats it as the device's firmware image.

Validation does not catch it: `size > 10 && front==0xF0 && back==0xF7 && KMI header matches`
(`src/SendSysEx.cpp:1873`) is satisfied by the request itself, since it carries the same header.
Confirmed on the 2026-09-06 Linux run — the captured "image" was byte-identical to
`kSoftStepDumpRequest`, all 75 bytes.

**Why it matters:** the dump is the safety gate before a trojan install on bootloader-less hardware.
A false pass means the trojan gets flashed to a device whose real image was never read.

**Fixed by:** (1) `openDeviceInputPorts()` restricts listeners to ports carrying the family marker,
so loopbacks are never opened; (2) a capture equal to the request just sent is rejected; (3) dump
validation requires ≥ 4096 bytes. Verified on hardware: the version query now reads **v93** (was
`version ?`) and the dump gate captures **66,312 bytes** (was the 75-byte echo).

## Linux/ALSA: `--bootloader-install softstep` fails

**Status: RESOLVED 2026-09-06 — full end-to-end success on Linux, uncommitted (awaiting approval).**
Originally suspected as ALSA gating incomplete SysEx. That was half right: ALSA was not gating
*inbound* SysEx, but it *was* rejecting *outbound* fragments. Three separate faults, all now fixed:

1. **The RX code bug above** — `Midi Through` echoed the request back and it false-passed validation.
   This is what broke the original run. **Fixed.**
2. **ALSA TX rejected the legacy sector send.** `MidiOutAlsa::sendMessage()` re-encodes through
   `snd_midi_event_encode()`, which rejects any span that is not a self-contained message — and the
   sector spans are deliberately fragments of one large SysEx. **Fixed** by adding
   `sendMessageFragment()` to RtMidi (`snd_seq_ev_set_sysex()`, bypassing the encoder); default
   forwards to `sendMessage()` so macOS/Windows are unchanged. See `decisions.md`.
3. **An incomplete alsa-lib install** — the source build of alsa-lib 1.2.14 at
   `/usr/local/lib/libasound.so.2` (installed for MIDI 2.0 / UMP support) was configured with no
   `--prefix`, so it baked in `ALSA_PLUGIN_DIR = /usr/local/lib/alsa-lib` — a directory that never
   got created, because `alsa-plugins` was not also built. The distro `/etc/alsa/conf.d/99-pulse.conf`
   then asked for `libasound_module_conf_pulse.so` by bare name, the lookup failed, and config parsing
   aborted before the sequencer was reachable — breaking *every* ALSA client, `aconnect -l` included.
   **Fixed** by `scripts/fix-alsa-plugin-dir.sh`, which symlinks `/usr/local/lib/alsa-lib` at the
   distro plugin dir, keeping 1.2.14 (and its MIDI 2.0 / UMP tools) in place. Environmental, not a
   SendSysEx bug.

**Hardware-validated end-to-end on Linux (2026-09-06)**: SoftStep legacy v93 → trojan install
(100/100 sectors, zero errors) → `VERIFY OK`, bootloader v1.0.0 → `--fw-update softstep` (290/290
chunks ACKed) → **application v2.0.7 confirmed**, device enumerating as `1f38:000c`. All three faults
are now fixed; SendSysEx runs natively against alsa-lib 1.2.14 with **no `LD_LIBRARY_PATH`
workaround**.

## Code-signing certificate is office-only

Windows releases must be signed with the KMI SafeNet token, which is physically at the office.
Remote sessions cannot produce signed releases. **Unblocked by:** being at the office or arranging
remote signing access.

## No automated hardware-in-the-loop tests

Manual smoke-test on real devices is the only validation path. Adding CI would require hardware
fixtures. **Needs:** hardware test rig decision from the team.

---

## Resolved

- **Windows validation of the fw-update reliability fixes** — RESOLVED. Re-run on real hardware over
  both WMS and WinMM; no regressions. This was the v0.15.0 release gate.
- **macOS release packaging** — RESOLVED (2026-08-27). `package-release-macos.sh` builds a universal
  signed/notarized/stapled `.pkg` installer; documented in `RELEASING.md` → macOS. (A drag `.dmg` was
  shipped for v0.14.0/v0.15.0, then replaced when the loose-CLI Gatekeeper block surfaced — see
  `decisions.md`.)
- **Stale README version header** — RESOLVED. README and `CMakeLists.txt` both read 0.15.0.
