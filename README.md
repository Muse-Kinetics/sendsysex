# Send SysEx #

Command-line utility to send MIDI SysEx messages, with support for two-step bootloader firmware updates.

Eric Bateman  
eric@musekinetics.com

Version 1.0.0  
Last updated: 2026-10-15

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

Requires Visual Studio 2022 or later with **Desktop development with C++** (a Windows
SDK 10.0.22000 or later, which includes C++/WinRT), and CMake 3.24 or later. Check
with `cmake --version`; if it's older, use the CMake bundled with Visual Studio
(e.g. `<VS install>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`)
instead of whatever is first on PATH.

To build with Windows MIDI Services (recommended, and required for the bootloader
installs), first get Microsoft's `Windows.Devices.Midi2` package and tell CMake where
it is (see [Building with Windows MIDI Services](#building-with-windows-midi-services)).
Without it, CMake says so and builds with WinMM only.

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

#### Building with Windows MIDI Services

`inc/rtmidi` is pinned to the Muse-Kinetics fork of RtMidi (RtMidi 6.1.0 plus the
`int` return from `sendMessage()` and `drain()`). Its Windows MIDI Services (WMS)
backend uses Microsoft's in-box `Windows.Devices.Midi2` API. Windows doesn't include
that API yet, so until it does (see [Windows MIDI Services](#windows-midi-services)
below) it comes from Microsoft's NuGet package, and SendSysEx ships Microsoft's DLL
next to `SendSysEx.exe`. RtMidi's
[WINDOWS.md](https://github.com/thestk/rtmidi/blob/master/WINDOWS.md) has the details.

1. From the current [Windows MIDI Services release](https://github.com/microsoft/MIDI/releases)
   (In-box Preview 11 as of October 2026), download
   `Windows.Devices.Midi2.<version>.nupkg`, e.g.
   `Windows.Devices.Midi2.0.99.92-preview.11.nupkg`.
2. Unpack it somewhere outside this repo. A `.nupkg` is a zip file; in cmd or
   PowerShell, Windows' own `tar` reads it (the `tar` in Git Bash can't):

   ```
   mkdir C:\sdk\wms-preview11
   tar -xf Windows.Devices.Midi2.0.99.92-preview.11.nupkg -C C:\sdk\wms-preview11
   ```

3. Point CMake at it, either once per build directory or with an environment
   variable (which also covers the preset and `package-release.ps1`):

   ```
   cmake -B build -DWindowsMidiServices_ROOT=C:/sdk/wms-preview11
   setx WindowsMidiServices_ROOT C:\sdk\wms-preview11
   ```

Configure prints `Windows MIDI Services: bundling <path>` when it found the package,
and each build copies `Windows.Devices.Midi2.dll` and `Windows.Devices.Midi2.pri`
from the package's `runtimes/win-x64/native` next to `SendSysEx.exe`. Keep the two
files together with the exe; without them SendSysEx uses WinMM. There is no 32-bit
WMS, so a 32-bit build is WinMM only. A build directory that was configured against
Microsoft's older out-of-band SDK forgets its now-missing path on the next configure.

#### Choosing the MIDI backend

On Windows, SendSysEx uses WMS when it was built in and can run on this PC, and
WinMM otherwise. It prints the backend it picked, and why, right after the version
banner. Set `KMI_MIDI_BACKEND=winmm` in the environment to force WinMM (useful for
testing the WinMM path), or pass `--midi-backend <winmm|wms>` to choose per run (see
`SendSysEx --help`). Forcing `wms` fails immediately, with the reason, if WMS isn't
available, rather than falling back to WinMM.

---

## Windows MIDI Services

Windows MIDI Services is Microsoft's new MIDI system for Windows 11. SendSysEx uses
it where it can and the classic Windows MIDI support (WinMM) everywhere else, so
firmware updates work either way. The **bootloader installs**
(`--bootloader-install`) need Windows MIDI Services: under WinMM the device can drop
off USB partway through.

Plug the device straight into the computer for a bootloader install, not into a hub.
We've seen a USB 2.0 hub drop a SoftStep halfway through one, which can leave it unable
to start. On Windows, SendSysEx shows how the device is connected and asks before
installing through a hub. Inside a virtual machine it can't see hubs on the host
computer, so check those yourself.

**What you need**

- Windows 11 25H2 or later, kept up to date. On older Windows, SendSysEx uses WinMM.
- Nothing else to install. SendSysEx uses a **preview** of Microsoft's Windows MIDI
  Services API, and carries its own copy: keep `Windows.Devices.Midi2.dll` and
  `Windows.Devices.Midi2.pri` in the same folder as `SendSysEx.exe`.

**Checking it works**

Run `SendSysEx -l`. The `MIDI backend:` line near the top says
`Windows MIDI Services` when it's in use, or which backend it chose instead and why.
The usual reasons:

- *The Windows MIDI Services API ... is missing*: Windows is older than 11 25H2, or
  the two files above aren't next to `SendSysEx.exe`.
- *Windows is set to use its legacy MIDI stack*: Microsoft explains how to switch in
  [How to change API mode](https://microsoft.github.io/MIDI/kb/how-to-change-api-mode/).
- *The Windows MIDI Service is not running*: restart the PC; if that doesn't help,
  ask on Microsoft's channels below.

**Optional extras from Microsoft:** the **Tools** package (MIDI Settings, a monitor and
troubleshooting tools) and the **Transports** package (loopback, network and
Bluetooth MIDI), both on the [release page](https://github.com/microsoft/MIDI/releases).
Microsoft asks you to uninstall older previews from Settings > Apps first.

**After Microsoft's November 2026 Windows update**, Windows 11 25H2 and later include
the API through Windows Update. SendSysEx then uses Windows' copy automatically, and
a later SendSysEx release will stop shipping its own.

For problems with Windows MIDI Services itself, Microsoft answers on the
[Windows MIDI Services Discord](https://aka.ms/mididiscord) and their
[issue tracker](https://github.com/microsoft/MIDI/issues).

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

# Guided bootloader install for older units that don't have one yet
SendSysEx --bootloader-install <family>
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

- [RtMidi](https://www.music.mcgill.ca/~gary/rtmidi/), Muse-Kinetics fork (6.1.0 plus `int sendMessage()` and `drain()`), pinned as the `inc/rtmidi` submodule
- [MIDI_CPP](https://github.com/Muse-Kinetics/midi_cpp) — pinned as the `lib/MIDI_CPP` submodule; SysEx framing and device-metadata parsing, compiled directly into the binary
- [nlohmann/json](https://github.com/nlohmann/json) — pinned as the `lib/json` submodule (header-only); parses the family database JSON in `data/families/`
- [MIDI-Sysex-MFG-IDs](https://github.com/insolace/MIDI-Sysex-MFG-IDs) — pinned as the `lib/syxMfg` submodule; manufacturer-ID lookup table used by `--id-request`'s printed identity reply
- **macOS:** CoreMIDI, CoreAudio, CoreFoundation (linked automatically by CMake)
- **Windows:** WinMM, linked automatically by CMake; Windows MIDI Services from
  Microsoft's `Windows.Devices.Midi2` NuGet package (see
  [Building with Windows MIDI Services](#building-with-windows-midi-services))