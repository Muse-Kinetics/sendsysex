#include "midiBackend.h"
#include <vector>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/utsname.h>
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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
    // macOS / Linux: resolve the concrete compiled-in API rather than leaving it
    // UNSPECIFIED (which prints as "Unknown"). getCompiledApi() returns the
    // available APIs in the same priority order RtMidi's own UNSPECIFIED search
    // uses, so picking the first non-dummy matches what actually gets constructed
    // (CoreMIDI on macOS, ALSA/JACK on Linux) without changing behavior.
    std::vector<RtMidi::Api> apis;
    RtMidi::getCompiledApi(apis);
    for (size_t i = 0; i < apis.size(); ++i)
    {
        if (apis[i] != RtMidi::UNSPECIFIED && apis[i] != RtMidi::RTMIDI_DUMMY)
            return Selection{apis[i], "auto-detected", true};
    }
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

namespace
{
#if defined(__APPLE__) || defined(__linux__)
// Read the first line of a command's stdout, trimmed. "" on any failure.
std::string readCommand(const char *cmd)
{
    FILE *p = popen(cmd, "r");
    if (!p)
        return std::string();
    char buf[256] = {0};
    std::string out;
    if (fgets(buf, sizeof(buf), p))
        out = buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}
#endif

#if defined(__linux__)
// PRETTY_NAME="..." from /etc/os-release, unquoted. "" if unavailable.
std::string linuxDistroPretty()
{
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line))
    {
        const std::string key = "PRETTY_NAME=";
        if (line.compare(0, key.size(), key) == 0)
        {
            std::string v = line.substr(key.size());
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
                v = v.substr(1, v.size() - 2);
            return v;
        }
    }
    return std::string();
}
#endif
}

std::string describeOs()
{
#if defined(__APPLE__)
    const std::string ver = readCommand("sw_vers -productVersion 2>/dev/null");
    return ver.empty() ? std::string("macOS (unknown version)") : ("macOS " + ver);
#elif defined(_WIN32)
    // GetVersionEx lies past Win8 without a manifest; RtlGetVersion (ntdll) gives
    // the true build. The marketing release ("25H2") lives in the registry's
    // DisplayVersion; 11 vs 10 is build >= 22000.
    std::string result = "Windows";
    typedef LONG(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.dwOSVersionInfoSize = sizeof(info);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionPtr rtlGetVersion =
        ntdll ? reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(ntdll, "RtlGetVersion")) : 0;
    unsigned long build = 0;
    if (rtlGetVersion && rtlGetVersion(&info) == 0)
    {
        build = info.dwBuildNumber;
        result += (info.dwMajorVersion >= 10 && build >= 22000) ? " 11" : " 10";
    }
    char display[64] = {0};
    DWORD sz = sizeof(display);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     "DisplayVersion", RRF_RT_REG_SZ, 0, display, &sz) == ERROR_SUCCESS && display[0])
        result += std::string(" ") + display;
    if (build)
        result += " (build " + std::to_string(build) + ")";
    return result;
#elif defined(__linux__)
    struct utsname u;
    std::string kernel = (uname(&u) == 0) ? u.release : std::string();
    std::string base = kernel.empty() ? std::string("Linux") : ("Linux " + kernel);
    const std::string pretty = linuxDistroPretty();
    return pretty.empty() ? base : (base + " (" + pretty + ")");
#else
    return "unknown OS";
#endif
}
}
