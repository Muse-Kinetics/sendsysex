# Guidelines

## Code

- C++11 standard (`CMAKE_CXX_STANDARD 11`). Do not use C++14/17 features.
- No external runtime dependencies beyond the OS MIDI stack and the KMI RtMidi submodule.
- `SendSysEx --help` is the authoritative CLI reference — keep it in sync with argument parsing in `src/SendSysEx.cpp`.
- Device knowledge belongs in `data/`, not hardcoded in C++.

## Device Database & Family JSONs

- All new device families go in `data/families/<family>.json` following the schema in `data/schemas/kmi_family.schema.json`.
- Firmware-update quirks are tuned per family via `transport.firmwareUpdateDefaults` (timing:
  `firstChunkSize`/`firstGapDelayMs`/`chunkDelayMs`/`postDelayMs`; behavior: `rebootsToAppOnFinalChunk`,
  `confirmByAppReconnectOnly`) and `identity.versionEncoding` (`standard` | `bcd16`). Each is
  documented in the schema and defaults to prior behavior when absent — always add an inline
  `notes[]` entry recording the hardware finding that justified the value.
- Firmware payloads go in `syx/<family>/`.
- `data/kmi_device_database.json` aggregates across families — update it when adding a new family.
- Increment `schemaVersion` in the family JSON if the schema changes.

## Versioning

- Version lives only in `CMakeLists.txt` (`project(SendSysEx VERSION X.Y.Z)`). Update README.md `Version` line to match when bumping.
- Use semver. Tag releases as `vX.Y.Z`.

## Release Process

- Windows releases require the SafeNet signing token (office-only). See `RELEASING.md`.
- macOS builds are unsupported/undocumented for public release as of v0.13.0.
- Run `package-release.ps1` from a Developer PowerShell with MSVC on PATH.

## Submodules

- `inc/rtmidi` — KMI WMS fork. Do not update to upstream RtMidi.
- `lib/MIDI_CPP` — update only with intention.
- Always run `git submodule update --init --recursive` after cloning or pulling.

## Commits

- Commit messages are imperative present-tense and specific (e.g., "Add SoftStep v2.0.7 payload").
- Include Co-authored-by trailer for agent commits.

## Project State Files

- Keep `.buddy-project/` files up to date as decisions are made and tasks change.
- `blockers.md` should reflect real current blockers, not historical ones.
