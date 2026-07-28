#include "midiBackend.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace midiBackend
{
namespace
{
std::string toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool winmmForcedByEnv()
{
    const char *value = std::getenv("KMI_MIDI_BACKEND");
    return value != 0 && toLower(value) == "winmm";
}

struct Selection
{
    RtMidi::Api api;
    std::string reason;
    bool valid;
};

Selection &mutableSelection()
{
    static Selection s = {RtMidi::UNSPECIFIED, "", false};
    return s;
}

Selection autoDetect()
{
#if defined(__WINDOWS_MIDI_SERVICES__) && defined(__WINDOWS_MM__)
    if (winmmForcedByEnv())
        return Selection{RtMidi::WINDOWS_MM, "KMI_MIDI_BACKEND=winmm set - forcing WinMM", true};
    if (RtMidi::isWindowsMidiServicesAvailable())
        return Selection{RtMidi::WINDOWS_MIDI_SERVICES, "Windows MIDI Services SDK detected", true};
    return Selection{RtMidi::WINDOWS_MM, "Windows MIDI Services SDK not installed - falling back to WinMM", true};
#elif defined(__WINDOWS_MIDI_SERVICES__)
    return Selection{RtMidi::WINDOWS_MIDI_SERVICES, "built with Windows MIDI Services only", true};
#elif defined(__WINDOWS_MM__)
    return Selection{RtMidi::WINDOWS_MM, "built with WinMM only", true};
#else
    return Selection{RtMidi::UNSPECIFIED, "platform default backend", true};
#endif
}
}

bool initialize(const std::string &forcedBackend, std::string &errorMessage)
{
    errorMessage.clear();

    if (forcedBackend.empty())
    {
        mutableSelection() = autoDetect();
        return true;
    }

#if !defined(_WIN32)
    (void)errorMessage;
    errorMessage = "--midi-backend is only supported on Windows.";
    return false;
#else
    const std::string requested = toLower(forcedBackend);

    if (requested == "winmm")
    {
#if defined(__WINDOWS_MM__)
        mutableSelection() = Selection{RtMidi::WINDOWS_MM, "--midi-backend winmm forced", true};
        return true;
#else
        errorMessage = "This build was not compiled with WinMM support.";
        return false;
#endif
    }

    if (requested == "wms")
    {
#if defined(__WINDOWS_MIDI_SERVICES__)
        if (!RtMidi::isWindowsMidiServicesAvailable())
        {
            errorMessage = "--midi-backend wms was requested, but the Windows MIDI Services SDK "
                            "runtime is not installed/running on this machine.";
            return false;
        }
        mutableSelection() = Selection{RtMidi::WINDOWS_MIDI_SERVICES, "--midi-backend wms forced", true};
        return true;
#else
        errorMessage = "This build was not compiled with Windows MIDI Services support.";
        return false;
#endif
    }

    errorMessage = "Invalid --midi-backend value '" + forcedBackend + "'. Use 'winmm' or 'wms'.";
    return false;
#endif
}

RtMidi::Api selectedApi()
{
    Selection &s = mutableSelection();
    if (!s.valid)
        s = autoDetect();
    return s.api;
}

std::string describeSelectedApi()
{
    selectedApi(); // ensure mutableSelection() is populated
    return RtMidi::getApiDisplayName(selectedApi()) + " (" + mutableSelection().reason + ")";
}
}
