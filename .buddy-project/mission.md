# Mission

**SendSysEx** is KMI/Muse Kinetics' open-source command-line utility for sending MIDI SysEx messages and performing firmware updates on KMI/MK hardware devices.

## Purpose

- Send arbitrary `.syx` files to any MIDI port (raw mode)
- Automate two-step bootloader firmware updates for the full KMI device family
- Serve as the production firmware-update tool for end users and internal QA

## Scope & Constraints

- Cross-platform: macOS (CoreMIDI via RtMidi) and Windows (WinMM + Windows MIDI Services)
- Windows is the primary release target; macOS builds are developer/QA use
- Device knowledge lives in `data/families/*.json` + `data/kmi_device_database.json`; the CLI is generic
- The KMI-pinned RtMidi fork (`inc/rtmidi`) is a hard dependency — do not replace or generalize it
- MIT licensed; public GitHub repo

## Success Criteria

- Reliable firmware update for all supported KMI families (12 Step, BopPad, K-Board, KBP4, MalletStation, MimicHub, QuNeo, QuNexus, SoftStep, SoundStation)
- Zero regressions on Windows WinMM and WMS backends
- Minimal binary with no runtime deps beyond the OS MIDI stack
