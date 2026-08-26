# Current Task

## Status: firmware-update reliability hardening on macOS — DONE, hardware-validated; Windows validation + version bump + release next

Branch `bootloader_ug` (uncommitted at time of writing → being committed/pushed now for
Windows validation). Version still `0.13.0` in CMakeLists; **the next release must bump it**
(see handoff.md "Before the next release").

### This session (2026-08-25) — chunked `--fw-update` made reliable across the whole family, on macOS/CoreMIDI

Root-caused and fixed a class of `--fw-update` failures that surfaced after the Windows work
(`840f09d`) was pulled onto macOS. All fixes are in the tool + `data/families/*.json` — **no
MIDI_CPP change**. Validated single-pass on real hardware for every attached device:

| Family | Fixes | Result |
|---|---|---|
| SoftStep | `firstGapDelayMs`, `rebootsToAppOnFinalChunk` | ✅ v2.0.7 |
| BopPad | `firstGapDelayMs`, `rebootsToAppOnFinalChunk` | ✅ v3.0.4 |
| K-Board | `rebootsToAppOnFinalChunk` (already had erase settings) | ✅ v1.2.2 |
| QuNexus | `firstGapDelayMs`, `rebootsToAppOnFinalChunk` | ✅ v2.2.1 |
| 12 Step | `firstGapDelayMs`, `rebootsToAppOnFinalChunk` (modern path, NOT trojan) | ✅ v1.0.9 |
| QuNeo | `firstGapDelayMs`, `chunkDelayMs: 700`, `rebootsToAppOnFinalChunk`, `versionEncoding: "bcd16"` | ✅ v1.2.31 |
| MalletStation | `firstGapDelayMs`, `rebootsToAppOnFinalChunk`, `confirmByAppReconnectOnly` | ✅ (reconnect-confirmed) |
| KBP4 | none (single-monolithic-message path, unaffected) | ✅ v1.2.2 |

See decisions.md for the four new mechanisms and *why*. Every JSON change carries an inline
`notes[]` entry with the hardware finding.

### Also this session — `--bootloader-install` ported to 12 Step (hardware-validated)

`--bootloader-install 12step` now works end-to-end, identical methodology to SoftStep (same-era
trojan install). Confirmed on a real v28 unit: legacy detection → consent gate → 5× version check →
firmware-dump validation → trojan install → verify (rebooted into the 12 Step bootloader) → offer to
load latest firmware. The port was straightforward — only the device-specific IDs differ:
- Added `k12StepDumpRequest` (byte-identical to SoftStep's except header `01 55 7A 14`, from the
  verified `12s_firmware_update_request.syx`).
- Generalized `captureFirmwareDump` (family-selected request bytes + port marker) and
  `runBootloaderInstall` (family display name, banner, dump-header validation, `--bl-send`/
  `--fw-update` handoffs). Everything else was already generic: the `bootloader_installer_legacy`
  payload (md5-verified trojan, 2 banks/98 sectors, all CRCs valid), the `legacy_application` port
  profile, the version query, `familyFromImage`, and `--verify`.

### Priority Order (Next)
1. **Windows validation** (owner/SiliconDreams-Windows): re-run `--fw-update` for each family on
   WMS and WinMM. The new JSON flags are read on all platforms; the `drain()` pacing is a no-op on
   Windows (WinMM/WMS inherit the base no-op `drain()`). Expected same or better behavior.
2. **Reconcile the `WMS` branch** (`origin/WMS`, `56580a7`): it has a cmake/rtmidi refactor
   ("use rtmidi add_subdirectory; drop redundant WMS wiring") NOT in `bootloader_ug`. Decide
   whether it merges before the release. See handoff.md "Branch landscape".
3. **Version bump + release** — see handoff.md "Before the next release" for the full checklist
   (bump CMakeLists, sync README 0.11.0→new, changelog, tag, sign on Windows).
4. EM Pro Riser: shares MalletStation's EM1 firmware → will need `confirmByAppReconnectOnly` if/when
   it gets a `--fw-update` family entry.

### Phase
Firmware-update reliability hardening → pre-release.
