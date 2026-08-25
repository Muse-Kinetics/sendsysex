#ifndef MIDI_BACKEND_H
#define MIDI_BACKEND_H

#include <string>

#include "RtMidi.h"

namespace midiBackend
{
// Explicitly selects the MIDI backend for the life of the process, honoring
// a --midi-backend CLI override. Call once from main(), before any other
// midiBackend function or any RtMidiIn/RtMidiOut is constructed.
//   forcedBackend: "" to auto-detect (mirroring the SoftStep editors -
//                  shared/KMI_MDM/KMI_ports.cpp: WMS if its SDK runtime is
//                  installed and running, else WinMM; KMI_MIDI_BACKEND=winmm
//                  in the environment still forces WinMM in this mode),
//                  "winmm" to force WinMM, or "wms" to force Windows MIDI
//                  Services.
// Forcing a backend is Windows-only. Forcing "wms" hard-fails (returns
// false) if the WMS SDK runtime isn't installed/running, rather than
// silently falling back to WinMM the way auto-detect does - the caller
// asked for WMS specifically. On failure, errorMessage explains why and the
// backend is left unselected (selectedApi() falls back to auto-detect).
bool initialize(const std::string &forcedBackend, std::string &errorMessage);

// The backend selected by the most recent successful initialize() call. If
// initialize() was never called (or failed), lazily auto-detects on first
// use, so this is always safe to call. Every RtMidiIn/RtMidiOut in this
// program should be constructed with this API so all of them agree on the
// same backend.
RtMidi::Api selectedApi();

// Human-readable description of the selected backend, e.g. "Windows MIDI
// Services" or "Windows Multimedia (WinMM)", for the CLI startup banner.
std::string describeSelectedApi();

// Human-readable host OS + version for the CLI startup banner, e.g.
// "macOS 26.3", "Windows 11 25H2 (build 26100)", "Linux 6.3 (Ubuntu 24.04)".
// Best-effort; falls back to a coarse label if the exact version can't be read.
std::string describeOs();
}

#endif
