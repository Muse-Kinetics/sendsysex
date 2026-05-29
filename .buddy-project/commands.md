---
*Imported from `docs\windows-midi-services-sdk-not-installed.md` on 2026-05-25*

# Windows MIDI Services SDK Runtime — Not Installed Error

## Error description

When the `winmidi2` RtMidi backend is active and the Windows MIDI Services
**SDK runtime DLL** has not been installed, the following message is emitted:

```
WinMidiServicesClass: Windows MIDI Services SDK runtime not available.
Install the runtime from https://aka.ms/MidiServicesLatestSdkRuntimeInstaller
```

There is no numeric error code from the RtMidi layer.  Internally,
`MidiDesktopAppSdkInitializer::InitializeSdkRuntime()` returns `false` when the
runtime COM object (`MidiDesktopAppSdkRuntimeComponent`) cannot be found in the
Windows registry.

---

## Why this error occurs

Windows MIDI Services is delivered in **two distinct parts**:

| Part | How it arrives | Present on this machine |
|------|---------------|------------------------|
| MIDI service + Midi2.*.dll transport plugins + MidiSrv.exe | In-box with Windows 11 24H2 / 25H2 / 26H1 | ✅ Yes |
| **App SDK runtime DLL** (`Microsoft.Windows.Devices.Midi2.dll`) | Out-of-band installer (separate download) | ❌ Not yet |

The out-of-band SDK runtime is what our application links against at runtime via
the bootstrapper (`MidiDesktopAppSdkInitializer`).  Without it, the service is
running but cannot be reached through the SDK API.

---

## Distinguishing this error in code

### RtMidi layer (`RtMidiError::Type`)

The error is reported with type `RtMidiError::DRIVER_NOT_INSTALLED` (added in
this project's fork of RtMidi).  This is distinct from:

| Type | Meaning |
|------|---------|
| `WARNING` | Soft, non-fatal advisory |
| `DRIVER_ERROR` | Driver present but a runtime call failed |
| `DRIVER_NOT_INSTALLED` | **Required driver / runtime is absent** |

The error does **not throw** — it prints to `stderr` like a `WARNING` so that
object construction does not crash callers that have not set an error callback.

### Proactive check (`RtMidi::checkApiAvailability`)

The preferred way for applications to detect this condition **before** creating
any MIDI objects:

```cpp
RtMidi::RtMidiApiAvailability avail =
    RtMidi::checkApiAvailability(RtMidi::WINDOWS_MIDI_SERVICES);

if (!avail.available)
{
    // avail.message    = "Windows MIDI Services SDK runtime is not installed."
    // avail.installUrl = "https://aka.ms/MidiServicesLatestSdkRuntimeInstaller"
    showInstallDialog(avail.message, avail.installUrl);
}
```

### kmiDevice layer

`kmiDevice::refreshPorts()` calls `checkApiAvailability` internally.  On failure:

| Method | Value |
|--------|-------|
| `refreshPorts()` | returns `false` |
| `getState()` | `State::disconnected` |
| `requiresSdkInstall()` | `true` |
| `getSdkInstallUrl()` | `"https://aka.ms/MidiServicesLatestSdkRuntimeInstaller"` |
| `getLastError()` | `"Windows MIDI Services SDK runtime is not installed."` |

---

## User-facing message (`SendSysEx.cpp`)

When the SDK is missing, all modes (`-l`, `--id-request`, `--fw-update`) print:

```
=================================================================
  Windows MIDI Services SDK runtime not installed
=================================================================
  Windows MIDI Services SDK runtime is not installed.
  Download: https://aka.ms/MidiServicesLatestSdkRuntimeInstaller

  Run the installer then re-launch this application.
=================================================================
```

In a GUI application, replace `printSdkInstallPrompt()` with a modal dialog
that includes an **Open download page** button pointing to `installUrl`.

---

## Resolution

1. Download the runtime installer from  
   <https://aka.ms/MidiServicesLatestSdkRuntimeInstaller>
2. Run it (no reboot required).
3. Re-launch the application.

End users on **retail Windows 11 24H2, 25H2, and 26H1** with Windows Update
enabled have the in-box MIDI service, but must still install this separate SDK
runtime package to use applications built against the WMS SDK.

Microsoft ships the runtime out-of-band so it can be updated independently of
Windows.  Once installed it receives updates through Windows Update.

---

## Files changed to implement this handling

| File | Change |
|------|--------|
| `inc/rtmidi/RtMidi.h` | Added `RtMidiError::DRIVER_NOT_INSTALLED` enum value; added `RtMidi::RtMidiApiAvailability` struct; added `RtMidi::checkApiAvailability()` static method |
| `inc/rtmidi/RtMidi.cpp` | `MidiApi::error()` handles `DRIVER_NOT_INSTALLED` without throwing; `WinMidiServicesClass::init_sdk()` uses `DRIVER_NOT_INSTALLED`; `RtMidi::checkApiAvailability()` implemented for WMS and non-WMS builds |
| `inc/kmiDevice.h` | Added `requiresSdkInstall()`, `getSdkInstallUrl()`, `sdkInstallRequired_`, `sdkInstallUrl_` |
| `src/kmiDevice.cpp` | Early `checkApiAvailability` call in `refreshPorts()`; new getters implemented |
| `src/SendSysEx.cpp` | `printSdkInstallPrompt()` helper; check in `runManualProcess`, `runIdentityRequest`, `runAutomaticProcess` |


---
*Imported from `Docs\windows-midi-services-sdk-not-installed.md` on 2026-05-25*

# Windows MIDI Services SDK Runtime — Not Installed Error

## Error description

When the `winmidi2` RtMidi backend is active and the Windows MIDI Services
**SDK runtime DLL** has not been installed, the following message is emitted:

```
WinMidiServicesClass: Windows MIDI Services SDK runtime not available.
Install the runtime from https://aka.ms/MidiServicesLatestSdkRuntimeInstaller
```

There is no numeric error code from the RtMidi layer.  Internally,
`MidiDesktopAppSdkInitializer::InitializeSdkRuntime()` returns `false` when the
runtime COM object (`MidiDesktopAppSdkRuntimeComponent`) cannot be found in the
Windows registry.

---

## Why this error occurs

Windows MIDI Services is delivered in **two distinct parts**:

| Part | How it arrives | Present on this machine |
|------|---------------|------------------------|
| MIDI service + Midi2.*.dll transport plugins + MidiSrv.exe | In-box with Windows 11 24H2 / 25H2 / 26H1 | ✅ Yes |
| **App SDK runtime DLL** (`Microsoft.Windows.Devices.Midi2.dll`) | Out-of-band installer (separate download) | ❌ Not yet |

The out-of-band SDK runtime is what our application links against at runtime via
the bootstrapper (`MidiDesktopAppSdkInitializer`).  Without it, the service is
running but cannot be reached through the SDK API.

---

## Distinguishing this error in code

### RtMidi layer (`RtMidiError::Type`)

The error is reported with type `RtMidiError::DRIVER_NOT_INSTALLED` (added in
this project's fork of RtMidi).  This is distinct from:

| Type | Meaning |
|------|---------|
| `WARNING` | Soft, non-fatal advisory |
| `DRIVER_ERROR` | Driver present but a runtime call failed |
| `DRIVER_NOT_INSTALLED` | **Required driver / runtime is absent** |

The error does **not throw** — it prints to `stderr` like a `WARNING` so that
object construction does not crash callers that have not set an error callback.

### Proactive check (`RtMidi::checkApiAvailability`)

The preferred way for applications to detect this condition **before** creating
any MIDI objects:

```cpp
RtMidi::RtMidiApiAvailability avail =
    RtMidi::checkApiAvailability(RtMidi::WINDOWS_MIDI_SERVICES);

if (!avail.available)
{
    // avail.message    = "Windows MIDI Services SDK runtime is not installed."
    // avail.installUrl = "https://aka.ms/MidiServicesLatestSdkRuntimeInstaller"
    showInstallDialog(avail.message, avail.installUrl);
}
```

### kmiDevice layer

`kmiDevice::refreshPorts()` calls `checkApiAvailability` internally.  On failure:

| Method | Value |
|--------|-------|
| `refreshPorts()` | returns `false` |
| `getState()` | `State::disconnected` |
| `requiresSdkInstall()` | `true` |
| `getSdkInstallUrl()` | `"https://aka.ms/MidiServicesLatestSdkRuntimeInstaller"` |
| `getLastError()` | `"Windows MIDI Services SDK runtime is not installed."` |

---

## User-facing message (`SendSysEx.cpp`)

When the SDK is missing, all modes (`-l`, `--id-request`, `--fw-update`) print:

```
=================================================================
  Windows MIDI Services SDK runtime not installed
=================================================================
  Windows MIDI Services SDK runtime is not installed.
  Download: https://aka.ms/MidiServicesLatestSdkRuntimeInstaller

  Run the installer then re-launch this application.
=================================================================
```

In a GUI application, replace `printSdkInstallPrompt()` with a modal dialog
that includes an **Open download page** button pointing to `installUrl`.

---

## Resolution

1. Download the runtime installer from  
   <https://aka.ms/MidiServicesLatestSdkRuntimeInstaller>
2. Run it (no reboot required).
3. Re-launch the application.

End users on **retail Windows 11 24H2, 25H2, and 26H1** with Windows Update
enabled have the in-box MIDI service, but must still install this separate SDK
runtime package to use applications built against the WMS SDK.

Microsoft ships the runtime out-of-band so it can be updated independently of
Windows.  Once installed it receives updates through Windows Update.

---

## Files changed to implement this handling

| File | Change |
|------|--------|
| `inc/rtmidi/RtMidi.h` | Added `RtMidiError::DRIVER_NOT_INSTALLED` enum value; added `RtMidi::RtMidiApiAvailability` struct; added `RtMidi::checkApiAvailability()` static method |
| `inc/rtmidi/RtMidi.cpp` | `MidiApi::error()` handles `DRIVER_NOT_INSTALLED` without throwing; `WinMidiServicesClass::init_sdk()` uses `DRIVER_NOT_INSTALLED`; `RtMidi::checkApiAvailability()` implemented for WMS and non-WMS builds |
| `inc/kmiDevice.h` | Added `requiresSdkInstall()`, `getSdkInstallUrl()`, `sdkInstallRequired_`, `sdkInstallUrl_` |
| `src/kmiDevice.cpp` | Early `checkApiAvailability` call in `refreshPorts()`; new getters implemented |
| `src/SendSysEx.cpp` | `printSdkInstallPrompt()` helper; check in `runManualProcess`, `runIdentityRequest`, `runAutomaticProcess` |
