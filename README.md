# Send SysEx #

Command-line utility to send MIDI SysEx messages, with support for two-step bootloader firmware updates.

Eric Bateman  
eric@musekinetics.com

Version 0.9.0  
Last updated: 2026-07-27

Licensed under the [MIT License](LICENSE).

See [RELEASING.md](RELEASING.md) for how to build, sign, and publish a Windows release.

---

## Building with CMake

CMake is the primary build system and works on both macOS and Windows.

### 1. Clone the repo and submodule

```sh
git clone <repo-url>
cd sendsysex
git submodule update --init --recursive
```

### 2. Configure and build

```sh
cmake -B build
cmake --build build
```

The binary is placed in `build/` (or `build/Debug/` / `build/Release/` on Windows with MSVC).

### Windows

Requires Visual Studio or the MSVC toolchain, plus the **Windows MIDI Services SDK**
(both backends are always compiled in; see below). Requires a recent CMake -
`cmake --preset` needs 3.19+, and the `Visual Studio 17 2022` generator name isn't
recognized by CMake versions older than ~3.21 even though `cmake_minimum_required`
only demands 3.18. Check with `cmake --version`; if it's older, use the CMake bundled
with Visual Studio/Qt (e.g. `<VS install>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`)
instead of whatever is first on PATH.

The simplest option is to use the preset:

```sh
cmake --preset windows-msvc
cmake --build --preset windows-release
```

Or, for an explicit Release build without presets:

```sh
cmake -B build
cmake --build build --config Release
```

You can also generate a VS solution directly:

```sh
cmake -G "Visual Studio 17 2022" -A x64 -B build/msvc
```

#### MIDI backend (WMS + WinMM)

`inc/rtmidi` is pinned to the Muse-Kinetics WMS fork/branch of RtMidi, the same one
the SoftStep editors use. On Windows, both the WinMM and Windows MIDI Services (WMS)
backends are compiled into every build; `SendSysEx` probes for WMS at startup and
falls back to WinMM automatically if the WMS SDK runtime isn't installed. The CLI
prints which backend it picked near the top of every run, right after the version banner.

Set `KMI_MIDI_BACKEND=winmm` in the environment to force WinMM even on a machine
where WMS is installed (useful for testing the WinMM path), or pass
`--midi-backend <winmm|wms>` on the command line to do the same per-invocation -
see `SendSysEx --help`. Forcing `wms` fails immediately if the SDK runtime isn't
available rather than silently falling back to WinMM.

Building with WMS support requires the
[Windows MIDI Services SDK](https://github.com/microsoft/MIDI) to be installed
(`cppwinrt.exe` and the `Microsoft.Windows.Devices.Midi2.winmd` files); CMake will
fail with a clear error at configure time if it can't find them.

---

## Usage

`SendSysEx --help` is the source of truth for every flag - it's audited to stay in
sync with the actual argument parser. Two modes:

```sh
# Raw send mode: send a file directly to a port
SendSysEx -p <port number> -f <file.syx>
SendSysEx -n <port name>  -f <file.syx>
SendSysEx -l

# Automatic update mode: family-based two-step bootloader firmware update
SendSysEx --fw-update <family> [--fw-version <version>]
SendSysEx --id-request <family>
```

**Example — single file:**
```sh
SendSysEx -p 1 -f BopPad.syx
```

**Example — automatic firmware update:**
```sh
SendSysEx --fw-update SoftStep
SendSysEx --fw-update BopPad --midi-backend winmm
```

---

## Dependencies

All required submodules; `git submodule update --init --recursive` (step 1 above) fetches them all.

- [RtMidi](https://www.music.mcgill.ca/~gary/rtmidi/), Muse-Kinetics WMS fork — pinned as the `inc/rtmidi` submodule
- [MIDI_CPP](https://github.com/Muse-Kinetics/midi_cpp) — pinned as the `lib/MIDI_CPP` submodule; SysEx framing and device-metadata parsing, compiled directly into the binary
- [nlohmann/json](https://github.com/nlohmann/json) — pinned as the `lib/json` submodule (header-only); parses the family database JSON in `data/families/`
- [MIDI-Sysex-MFG-IDs](https://github.com/insolace/MIDI-Sysex-MFG-IDs) — pinned as the `lib/syxMfg` submodule; manufacturer-ID lookup table used by `--id-request`'s printed identity reply
- **macOS:** CoreMIDI, CoreAudio, CoreFoundation (linked automatically by CMake)
- **Windows:** WinMM and Windows MIDI Services, both linked automatically by CMake; the
  WMS SDK must be installed to build (see above)