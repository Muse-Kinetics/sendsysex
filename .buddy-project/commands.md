# Commands

All commands confirmed from `CMakeLists.txt`, `CMakePresets.json`, `README.md`, and `RELEASING.md`.

## Prerequisites

```sh
git submodule update --init --recursive
```

## Build (macOS / Linux)

```sh
cmake -B build
cmake --build build
# Binary: build/SendSysEx
```

## Build (Linux)

```sh
cmake -B build && cmake --build build
./build/SendSysEx -l          # ALSA sequencer ports, e.g. "SSCOM:SSCOM MIDI 1 32:0"
```

The full firmware-update and bootloader-install path is hardware-validated on Linux as of 2026-09-06.

If every ALSA client fails with `Unknown SEQ default` / `Cannot open shared library
libasound_module_conf_pulse.so`, a source-built alsa-lib is pointing at a plugin dir that was never
populated — run:

```sh
./scripts/fix-alsa-plugin-dir.sh            # apply (prompts for sudo)
./scripts/fix-alsa-plugin-dir.sh --check    # report status only
./scripts/fix-alsa-plugin-dir.sh --undo     # revert
```

Useful when inspecting the ALSA MIDI path:

```sh
aconnect -l                   # list ALSA sequencer clients/ports
amidi -l                      # list raw ALSA MIDI devices
cat /proc/asound/seq/clients  # per-client input pool sizes (SysEx buffering)
```

## Build (Windows — preset)

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-release
# Binary: build\Release\SendSysEx.exe
```

## Build (Windows — explicit Release)

```powershell
cmake -B build
cmake --build build --config Release
```

## Run

```sh
./build/SendSysEx --help
./build/SendSysEx --help-bootloader               # legacy trojan-install help
./build/SendSysEx -l                              # list MIDI ports
./build/SendSysEx -p 0 -f path/to/file.syx        # send syx to port 0
./build/SendSysEx -n "SoftStep" -f file.syx       # send by port name
./build/SendSysEx --fw-update softstep            # firmware update (family is positional)
./build/SendSysEx --fw-update quneo --timestamp   # --timestamp prefixes every line with HH:MM:SS.mmm
./build/SendSysEx --id-request malletstation      # print the identity reply and exit
./build/SendSysEx --bl-send -f trojan.syx --family softstep --verify   # bootloader-trojan install
```

Note: `--fw-update <family>` takes the family as a positional argument (case-insensitive), NOT
`--family <family>`. `--family` is a flag used by `--bl-send` to auto-select the control-surface port.

## Force MIDI backend (Windows)

```sh
SendSysEx --midi-backend winmm ...
SendSysEx --midi-backend wms ...
# Or via environment:
set KMI_MIDI_BACKEND=winmm
```

## Release packaging (macOS - universal .pkg installer)

```sh
# Bump CMakeLists.txt VERSION + README.md first, then:
source ~/Desktop/refresh_keys.sh          # loads signing identities + notary profile
./package-release-macos.sh
# -> dist/SendSysEx-vX.Y.Z-macos-universal.pkg (+ .sha256)
# KMI_SKIP_NOTARIZE=1 for a signed-but-not-notarized local build.
```

Publish the macOS assets onto the existing release:

```sh
gh release upload vX.Y.Z \
    dist/SendSysEx-vX.Y.Z-macos-universal.pkg \
    dist/SendSysEx-vX.Y.Z-macos-universal.pkg.sha256
```

## Release packaging (Windows only)

```powershell
# Full build + stage + zip
.\package-release.ps1

# Re-zip after signing (skip rebuild)
.\package-release.ps1 -SkipBuild
```

## Tag and publish (after signing)

```powershell
git tag -a vX.Y.Z -m "SendSysEx vX.Y.Z"
git push origin vX.Y.Z
gh release create vX.Y.Z dist\SendSysEx-vX.Y.Z-win-x64.zip dist\SendSysEx-vX.Y.Z-win-x64.zip.sha256 --title "SendSysEx vX.Y.Z" --notes "..."
```

## No test suite

There are no automated tests. Validate manually by running `SendSysEx -l` and performing a firmware update on real hardware.
