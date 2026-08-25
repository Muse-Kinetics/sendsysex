# Blockers

## Windows validation pending for this session's reliability fixes (release gate)

The 2026-08-25 chunked-`--fw-update` fixes (drain pacing, `rebootsToAppOnFinalChunk`,
`versionEncoding: bcd16`, `confirmByAppReconnectOnly`) are hardware-validated on **macOS only**. The
JSON flags are read on every platform and the `drain()` pacing is a Windows no-op, so no regression
is expected — but it must be confirmed on real hardware over WMS and WinMM before the next release.
**Unblocked by:** a Windows session with the devices (owner will run this).

## Code-signing certificate is office-only

Windows releases must be signed with the KMI SafeNet token, which is physically at the office. Remote sessions cannot produce signed releases. **Unblocked by:** being at the office or arranging remote signing access.

## macOS release packaging is undocumented / unscripted

There is no `package-release.ps1` equivalent for macOS. macOS signing requires Apple Developer notarization, which has a separate credential chain. **Needs:** someone to define and document the macOS release flow.

## README version header is stale

README.md still reads `Version 0.11.0` but the project is at 0.13.0. **Unblocked by:** a one-line edit + commit (no human input required — agent can fix this).

## No automated hardware-in-the-loop tests

Manual smoke-test on real devices is the only validation path. Adding CI would require hardware fixtures. **Needs:** hardware test rig decision from the team.
