# Decisions

## Use the KMI WMS-fork of RtMidi (not upstream RtMidi)

`inc/rtmidi` is pinned to the Muse-Kinetics fork that adds Windows MIDI Services (WMS) support. This is the same fork the SoftStep editors use, so behavior is consistent. Upstream RtMidi does not support WMS. **Do not replace with upstream.**

## Both WinMM and WMS compiled into every Windows build

Rather than shipping separate WinMM and WMS binaries, both backends are always compiled in. The CLI probes for the WMS SDK runtime at startup and falls back to WinMM automatically. This mirrors the SoftStep editors and simplifies distribution.

## Per-family timing and identity live in JSON, not code

All firmware update timing (inter-chunk delay, ACK timeout, etc.) and device identity (PIDs, port name markers) are in `data/families/*.json`. This lets new families or timing adjustments be added without recompiling. Schema is versioned (`schemaVersion` field).

## `CMakeLists.txt` is the single version source of truth

`project(SendSysEx VERSION X.Y.Z)` flows into `version.h` via `configure_file`. README and release zips derive from this. Do not hardcode the version anywhere else.

## macOS release: universal notarized `.pkg` installer

macOS releases ship a **universal** (arm64 + x86_64) notarized **`.pkg`** built by
`package-release-macos.sh` (the counterpart to `package-release.ps1`). Decisions:

- **Always universal.** One download runs on both Apple Silicon and Intel. Built
  with `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`.
- **`.pkg`, not a `.dmg`.** This is the crux: a **loose CLI binary can't carry a
  stapled notarization ticket** (only `.app`/`.pkg`/`.dmg` can). A drag-install
  from a `.dmg` therefore hands the user a quarantined bare binary that Gatekeeper
  blocks ("developer cannot be verified" - hit in the field on v0.15.0). A `.pkg`
  installs to `/Applications/SendSysEx` **without** the quarantine flag, is itself
  stapled, and its postinstall symlinks `SendSysEx` into `/usr/local/bin` (PATH)
  and reveals the folder in Finder (via `launchctl asuser` so the window opens in
  the user's session, not root's). Double-clicks and installs clean, no `xattr`.
  (We briefly shipped a drag `.dmg` for v0.14.0/v0.15.0; replaced it with the
  `.pkg` once the Gatekeeper block surfaced. The old `create-dmg` path and its
  background asset were removed.)
- **Sign → notarize → staple.** Binary signed with the Kesumo **Developer ID
  Application** cert (hardened runtime + secure timestamp); the `.pkg` signed with
  the **Developer ID Installer** cert; notarized via a **notarytool keychain
  profile**, stapled, and validated. Identities + profile come from
  `~/Desktop/refresh_keys.sh` (`DEVELOPER_ID` / `DEVELOPER_ID_INSTALLER` /
  `APPLE_KEYCHAIN_PROFILE`) or `KMI_*` overrides - no Apple ID, password, or
  profile secret is stored in the repo. Full steps in `RELEASING.md` → macOS.

## Windows-only release packaging script

`package-release.ps1` handles the Windows build/stage/zip/checksum workflow. macOS releases require a manual process (documented in README, no script yet). The repo lives in a Dropbox-synced folder, which caused Dropbox locking issues during zip — staging happens in a temp dir outside the repo to avoid this.

## No automated test suite

All validation is manual smoke-test on real hardware. The firmware update state machine is device-state-dependent and cannot easily be unit-tested without hardware in the loop. Tests would require hardware fixtures — not currently set up.

## Firmware-update reliability: four mechanisms, all JSON-driven (2026-08-25)

Pulling the Windows work (`840f09d`) onto macOS surfaced a class of `--fw-update` failures. Root
causes and fixes (all in the tool + `data/families/*.json`, **no MIDI_CPP change** — the raw
byte→`version_t` mapping and the confirmation both already lived in `kmiDevice.cpp`):

1. **CoreMIDI `drain()` pacing** (`inc/chunkedSysExTransfer.h`). CoreMIDI's `sendMessage` is async;
   the id-request handshake queued behind still-draining data, so replies landed outside the reply
   window (deterministic failure ~chunk 6). Fix: `drain()` after the data chunk and after the id
   request, so the reply-wait measures a real window. **No-op on Windows** — WinMM/WMS inherit the
   base no-op `drain()`; only `MidiOutCore` overrides it. This is why it was safe to add without
   touching Windows behavior.

2. **`rebootsToAppOnFinalChunk`** (family flag). Devices that reboot to app mode as they commit the
   final chunk never answer an id request on the bootloader port afterward — the old code waited
   `postDelayMs` for a reply that can't come, then burned a 3s retry, before the app-port
   confirmation (~9s wasted). Flag skips the doomed final handshake and goes straight to the
   app-port reconnect + version check. Set for every chunked family whose device reboots
   (SoftStep/BopPad/K-Board/QuNexus/12Step/QuNeo/MalletStation). NOT KBP4 (monolithic path).

3. **`versionEncoding: "bcd16"`** (family flag, QuNeo only). QuNeo's identity reply sends only 2
   bytes per version — `[patch(LSB), (major<<4)|minor BCD nibbles (MSB)]` — vs the standard 3. A
   standard parse misaligns and BCD-misreads (QuNeo 1.2.31 → "18.0.0"), failing the version match
   on a healthy device. `bcd16` unpacks it correctly in `kmiDevice::midiCppIDReplyCallback`.
   Verified against `QuNeo_Firmware` Q/Q_Main.c + Q/Q_fwupdate_dem.h. **Decision: decode in the tool,
   not MIDI_CPP** (owner's call) — self-contained, no shared-submodule change.

4. **`confirmByAppReconnectOnly`** (family flag, MalletStation/EM-Pro-Riser). The EM1 firmware
   (`Marimba/Malletstation`, EM1 branch) has NO Universal Device Inquiry handler at all — it reports
   version only via a proprietary OSC-over-SysEx `/fw/w/version` message. So a standard id request in
   app mode never replies and the version-match confirmation can't pass. Flag confirms success by
   app-mode reconnect alone (detected by port name in `refreshPorts`, not an id reply) — the
   pre-`840f09d` behavior. A device stuck in the bootloader shows its bootloader port, so
   `state_==connected` still distinguishes success from failure.

All three flags live under `transport.firmwareUpdateDefaults` except `versionEncoding`, which is
under `identity`. All are documented in `data/schemas/kmi_family.schema.json` and default to the
prior behavior when absent.

## `--help` split into `--help` + `--help-bootloader` (2026-08-25)

Main `--help` keeps only everyday flags plus `--bootloader-install`/`--bootloader-port`; the legacy
trojan tooling (`--bl-send`, `--bl-decode`, `--send-bl-erase-reboot-cmd`, all the probe/gap/verify
options) moved to `--help-bootloader`. Keep both in sync with the parser in `src/SendSysEx.cpp`.

## Legacy bootloader-trojan workflow (SoftStep + 12 Step)

Pre-bootloader SoftStep (SSCOM-era) and pre-1.0.0 12 Step units cannot be updated via the normal
path. `--bl-send` / `--bootloader-install <family>` installs a one-time trojan payload that flashes a
bootloader into the device, after which normal `--fw-update` works. The trojan `.syx` files live in
`syx/SoftStep/` and `syx/12Step/` and are registered as each family's `bootloader_installer_legacy`
payload.

**SoftStep and 12 Step use the same install methodology** (same era, same design) — the flow in
`runBootloaderInstall`/`captureFirmwareDump` is family-parameterized and differs only by device IDs:
the KMI manufacturer/PID header (SoftStep `1B 48 7A 01` vs 12 Step `01 55 7A 14`) used in the legacy
version request, the firmware-dump request, and the dumped-image validation. Both are hardware-
validated end-to-end (SoftStep on v93, 12 Step on v28). Adding another same-era family is mostly:
register the trojan payload + a `legacy_application` port profile, add the family's header bytes to
`runLegacyVersionQuery`/`captureFirmwareDump`, and un-gate `runBootloaderInstall`.
