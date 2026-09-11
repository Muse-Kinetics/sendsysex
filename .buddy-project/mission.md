# Mission

**SendSysEx** is KMI/Muse Kinetics' open-source command-line utility for sending MIDI SysEx messages and performing firmware updates on KMI/MK hardware devices.

## Purpose

- Send arbitrary `.syx` files to any MIDI port (raw mode)
- Automate two-step bootloader firmware updates for the full KMI device family
- Serve as the production firmware-update tool for end users and internal QA

## Scope & Constraints

- **Platforms:**
  - macOS: CoreMIDI via RtMidi.
  - Windows: WinMM + Windows MIDI Services.
  - Linux: ALSA via RtMidi. It builds and runs, and the full firmware-update and bootloader-install
    path was hardware-validated there on 2026-09-06. It is **not yet a supported release target**;
    that's an open policy decision.
- Windows is the primary release target; macOS ships a notarized `.pkg` for developer/QA and field use
- Device knowledge lives in `data/families/*.json` + `data/kmi_device_database.json`; the CLI is generic
- The Muse-Kinetics RtMidi fork (`inc/rtmidi`, branch `sysex-send-flowcontrol`) is a hard dependency — do not replace it with upstream RtMidi
- MIT licensed; public GitHub repo

## Success Criteria

- Reliable firmware update for all supported KMI families (12 Step, BopPad, EM Pro, K-Board, KBP4, MalletStation, MimicHub, QuNeo, QuNexus, SoftStep, SoundStation)
- Zero regressions on Windows WinMM and WMS backends (validated on hardware as of v0.15.0)
- Minimal binary with no runtime deps beyond the OS MIDI stack
