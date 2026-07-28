# Send SysEx #

Command-line utility to send MIDI SysEx messages, with support for two-step bootloader firmware updates.

Eric Bateman  
eric@musekinetics.com

Version 0.9.0  
Last updated: 2026-07-27

Licensed under the [MIT License](LICENSE).

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
(both backends are always compiled in; see below). The simplest option is to use the preset:

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
prints which backend it picked as the first line of output.

Set `KMI_MIDI_BACKEND=winmm` in the environment to force WinMM even on a machine
where WMS is installed (useful for testing the WinMM path).

Building with WMS support requires the
[Windows MIDI Services SDK](https://github.com/microsoft/MIDI) to be installed
(`cppwinrt.exe` and the `Microsoft.Windows.Devices.Midi2.winmd` files); CMake will
fail with a clear error at configure time if it can't find them.

---

## Usage

```
SendSysEx -p <port number> -f <file.syx>
SendSysEx -n <port name>  -f <file.syx>
SendSysEx -l
```

**Flags:**

| Flag | Description |
|------|-------------|
| `-p <num>` | Select MIDI output port by number |
| `-n <name>` | Select MIDI output port by name |
| `-f <file>` | SysEx file to send |
| `-l` | List available MIDI output ports |
| `-b <name>` | Bootloader port name (enables bootloader mode) |
| `-bc <file>` | SysEx command to enter bootloader |
| `-t <seconds>` | Seconds to wait for bootloader port to appear |

**Example — single file:**
```sh
SendSysEx -p 1 -f BopPad.syx
```

**Example — bootloader + firmware update:**
```sh
SendSysEx -p 0 -bc 12Step-enter-bootloader.syx -b "12Step Bootloader" -t 3 -f 12Step_Firmware_v1.0.4.syx
```

---

## Dependencies

- [RtMidi](https://www.music.mcgill.ca/~gary/rtmidi/), Muse-Kinetics WMS fork — pinned as the `inc/rtmidi` submodule
- **macOS:** CoreMIDI, CoreAudio, CoreFoundation (linked automatically by CMake)
- **Windows:** WinMM and Windows MIDI Services, both linked automatically by CMake; the
  WMS SDK must be installed to build (see above)