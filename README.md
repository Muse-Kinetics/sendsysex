# Send SysEx #

Command-line utility to send MIDI SysEx messages, with support for two-step bootloader firmware updates.

Eric Bateman  
eric@musekinetics.com

2023-03-31

Licensed under the [MIT License](LICENSE).

---

## Building with CMake

CMake is the primary build system and works on both macOS and Windows.

```sh
cmake -B build
cmake --build build
```

The binary is placed in `build/` (or `build/Debug/` on Windows with MSVC).

### Windows

Requires Visual Studio or the MSVC toolchain. Build as above, or generate a VS solution:

```sh
cmake -G "Visual Studio 17 2022" -B build/msvc
```

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

- [RtMidi](https://www.music.mcgill.ca/~gary/rtmidi/) — included in `inc/rtmidi/`
- **macOS:** CoreMIDI, CoreAudio, CoreFoundation (linked automatically by CMake)
- **Windows:** winmm (linked automatically by CMake)