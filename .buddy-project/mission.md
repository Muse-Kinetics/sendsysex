# Send SysEx Mission

## Current Goal
Ship a stable Windows MIDI Services path in RtMidi (`winmidi2`) so SendSysEx can reliably complete firmware-update SysEx transfers.

## Near-Term Target
- Keep `build_winmidi2` as the primary target build.
- Treat chunk sizes below 256 bytes as the known-good operating range for short-term delivery.
- Keep `build_winmidi2_com` available for A/B comparison only.

## Active Scope
1. Verify reliable end-to-end sends on real update flows.
2. Keep WMS send-path changes small and testable.
3. Preserve existing build variants:
   - `build` (WinMM)
   - `build-uwp` (UWP)
   - `build_winmidi2` (WMS WinRT)
   - `build_winmidi2_com` (WMS COM raw)
4. Improve reproducibility of the winmidi2 build configuration for other engineers.

## Out of Scope
- External conversation transcripts
- Long historical debug logs in mission context
- Large architecture dumps already documented under `docs/`

## Notes
- The WMS backend exists in this repo's RtMidi fork; upstream RtMidi does not include this local WMS work.
- Mission content should stay concise and action-oriented.
