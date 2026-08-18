#include "deviceDatabase.h"

#include "deviceHelpers.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace
{
void appendUnique(std::vector<std::string> &target, const std::string &candidate)
{
    if (candidate.empty())
        return;

    if (std::find(target.begin(), target.end(), candidate) == target.end())
        target.push_back(candidate);
}

bool fileExists(const std::string &filePath)
{
    std::ifstream input(filePath.c_str(), std::ios::in | std::ios::binary);
    return input.good();
}

bool isAlsaAddressToken(const std::string &text)
{
    const std::size_t colonPos = text.find(':');
    if (colonPos == std::string::npos)
        return false;

    bool leftDigit = false;
    for (std::size_t i = 0; i < colonPos; ++i)
        if (std::isdigit(static_cast<unsigned char>(text[i])))
            leftDigit = true;
        else
            return false;

    bool rightDigit = false;
    for (std::size_t i = colonPos + 1; i < text.size(); ++i)
        if (std::isdigit(static_cast<unsigned char>(text[i])))
            rightDigit = true;
        else
            return false;

    return leftDigit && rightDigit;
}
}

deviceDatabase::deviceDatabase()
    : loaded_(false),
      probeOnly_(false),
      applicationPidMsb_(0),
      bootloaderPidMsb_(1),
      activeApi_(RtMidi::UNSPECIFIED)
{
}

deviceDatabase::deviceDatabase(const std::string &familyId)
    : loaded_(false),
      probeOnly_(false),
      applicationPidMsb_(0),
      bootloaderPidMsb_(1),
      activeApi_(RtMidi::UNSPECIFIED)
{
    if (!familyId.empty())
        loadFamily(familyId);
}

void deviceDatabase::clear()
{
    familyId_.clear();
    displayName_.clear();
    familyPath_.clear();
    lastError_.clear();
    loaded_ = false;
    probeOnly_ = false;
    applicationPidMsb_ = 0;
    bootloaderPidMsb_ = 1;
    familyMarkers_.clear();
    safeProbeApplicationRoles_.clear();
    safeProbeBootloaderRoles_.clear();
    inputRoleOrder_.clear();
    outputRoleOrder_.clear();
    firmwarePayloadVersions_.clear();
    knownPorts_.clear();
    portsByIndex_.clear();
    firmwareUpdateDefaults_ = FirmwareUpdateDefaults();
}

bool deviceDatabase::loadFamily(const std::string &familyId)
{
    clear();
    familyId_ = normalizeFamilyId(familyId);
    familyPath_ = resolveDataPath("data/families/" + familyId_ + ".json");
    if (familyPath_.empty())
    {
        lastError_ = "Could not locate family definition file for '" + familyId_ + "'.";
        return false;
    }

    std::ifstream fileStream(familyPath_.c_str());
    if (!fileStream.good())
    {
        lastError_ = "Family definition file is empty or unreadable: " + familyPath_;
        return false;
    }

    json root;
    try
    {
        fileStream >> root;
    }
    catch (const json::exception &e)
    {
        lastError_ = std::string("JSON parse error in ") + familyPath_ + ": " + e.what();
        return false;
    }

    displayName_         = root.value("name", std::string());
    applicationPidMsb_  = root.at("identity").value("stateDetection", json::object()).value("applicationPidMsb", 0);
    bootloaderPidMsb_   = root.at("identity").value("stateDetection", json::object()).value("bootloaderPidMsb", 1);

    {
        const json &fwDefaults = root.value("transport", json::object()).value("firmwareUpdateDefaults", json::object());
        firmwareUpdateDefaults_.firstChunkSize  = fwDefaults.value("firstChunkSize", 0U);
        firmwareUpdateDefaults_.firstGapDelayMs = fwDefaults.value("firstGapDelayMs", 0U);
        firmwareUpdateDefaults_.chunkDelayMs    = fwDefaults.value("chunkDelayMs", 0U);
        firmwareUpdateDefaults_.postDelayMs     = fwDefaults.value("postDelayMs", 0U);
    }

    for (const auto &m : root.at("discovery").at("familyMarkers"))
        appendUnique(familyMarkers_, m.get<std::string>());

    for (const auto &r : root.at("discovery").at("safeProbeRoles").value("application", json::array()))
        appendUnique(safeProbeApplicationRoles_, r.get<std::string>());
    for (const auto &r : root.at("discovery").at("safeProbeRoles").value("bootloader", json::array()))
        appendUnique(safeProbeBootloaderRoles_, r.get<std::string>());

    for (const auto &payload : root.value("payloads", json::array()))
    {
        if (payload.value("type", std::string()) == "firmware")
        {
            const std::string ver = payload.value("version", std::string());
            if (!ver.empty())
                appendUnique(firmwarePayloadVersions_, ver);
        }
    }

    // Build knownPorts_ / portsByIndex_ / role order from profiles.
    for (const auto &profile : root.value("profiles", json::array()))
    {
        const std::string stateValue      = profile.value("state", std::string());
        const bool isBootloaderProfile    = (stateValue == "bootloader");
        const json &portLayout            = profile.at("portLayout");

        std::vector<std::string> productStrings;
        for (const auto &ps : profile.at("productStrings"))
            productStrings.push_back(ps.get<std::string>());

        const std::string directions[2] = { "inputs", "outputs" };
        for (int di = 0; di < 2; ++di)
        {
            const std::string &dir = directions[di];
            if (!portLayout.contains(dir))
                continue;

            // A direction with exactly one port entry (single_port/unnamed_single_port
            // style, or a bootloader profile that only ever exposes one port even under
            // an otherwise multi-port "ordered" style) has no numbering ambiguity to
            // resolve: the real CoreMIDI port name for a single-port device is just the
            // bare product string, with no "Port 1" suffix (that suffix is a macOS/driver
            // convention for disambiguating the 2nd+ ports of a genuinely multi-port
            // composite device - see e.g. QuNexus's control_surface/expander/cv ports).
            // Appending it unconditionally previously made single-port families
            // unmatchable via family-marker port matching (confirmed on real hardware
            // with BopPad's "single_port" profile; K-Board's identically-shaped profile
            // has the same latent bug, just never exercised without --app-port).
            const bool singlePortDirection = (portLayout.at(dir).size() == 1);

            for (const auto &portEntry : portLayout.at(dir))
            {
                const int         portIndex = portEntry.value("index", -1);
                const std::string portRole  = portEntry.value("role",  std::string());
                const std::string portName  = portEntry.value("name",  std::string());

                // Role order: collect unique roles across all profiles in declaration order.
                if (di == 0)
                    appendUnique(inputRoleOrder_,  portRole);
                else
                    appendUnique(outputRoleOrder_, portRole);

                for (const std::string &productStr : productStrings)
                {
                    const std::string productLower = toLowerCopy(collapseWhitespaceCopy(trimCopy(productStr)));

                    std::string canonical;
                    if (!portName.empty())
                    {
                        canonical = toLowerCopy(collapseWhitespaceCopy(trimCopy(productStr + " " + portName)));
                    }
                    else if (portIndex >= 1 && !singlePortDirection)
                    {
                        std::ostringstream oss;
                        oss << productLower << " port " << portIndex;
                        canonical = oss.str();
                    }
                    else
                    {
                        canonical = productLower;
                    }

                    addKnownPort(canonical, portRole, isBootloaderProfile);
                    if (portIndex >= 1)
                        addPortByIndex(productLower, portIndex, canonical);
                }
            }
        }
    }

    if (knownPorts_.empty())
    {
        lastError_ = "Family definition did not produce any exact normalized port names.";
        return false;
    }

    loaded_ = true;
    return true;
}

bool deviceDatabase::loadFromName(const std::string &displayName)
{
    clear();
    familyId_ = normalizeFamilyId(displayName);
    displayName_ = displayName;
    familyMarkers_.push_back(displayName);
    safeProbeApplicationRoles_.push_back("main");
    safeProbeBootloaderRoles_.push_back("bootloader");
    probeOnly_ = true;
    loaded_ = true;
    return true;
}

bool deviceDatabase::isLoaded() const
{
    return loaded_;
}

const std::string &deviceDatabase::getFamilyId() const
{
    return familyId_;
}

const std::string &deviceDatabase::getDisplayName() const
{
    return displayName_;
}

const std::string &deviceDatabase::getLastError() const
{
    return lastError_;
}

bool deviceDatabase::getDefaultFirmwareVersion(std::string &version) const
{
    version.clear();
    if (familyPath_.empty())
        return false;

    std::ifstream fileStream(familyPath_.c_str());
    if (!fileStream.good())
        return false;

    json root;
    try { fileStream >> root; } catch (...) { return false; }

    std::string firstMatch, defaultMatch, latestMatch;

    for (const auto &payload : root.value("payloads", json::array()))
    {
        if (payload.value("type", std::string()) != "firmware")
            continue;
        const std::string v = payload.value("version", std::string());
        if (v.empty())
            continue;
        if (firstMatch.empty())
            firstMatch = v;
        if (payload.value("latest", false))
            latestMatch = v;
        if (payload.value("default", false))
            defaultMatch = v;
    }

    if (!latestMatch.empty())  { version = latestMatch;  return true; }
    if (!defaultMatch.empty()) { version = defaultMatch; return true; }
    if (!firstMatch.empty())   { version = firstMatch;   return true; }
    return false;
}

const deviceDatabase::FirmwareUpdateDefaults &deviceDatabase::getFirmwareUpdateDefaults() const
{
    return firmwareUpdateDefaults_;
}

bool deviceDatabase::isSupportedFirmwareVersion(const version_t &version) const
{
    // Compares parsed version_t structs, not raw strings: versionToString()
    // omits a trailing ".0" dev component, so a family whose JSON spells out
    // 4-component versions with dev=0 (e.g. KBP4's "1.2.2.0") would never
    // string-match itself after that round trip. version_t's operator== is
    // exact and format-independent - a JSON entry written as "2.0.4" and one
    // written as "2.0.4.0" compare equal here, unlike with strings. Found
    // 2026-08-15 when --fw-update KBP4 failed outright on its own default
    // version.
    for (const std::string &payloadVersion : firmwarePayloadVersions_)
    {
        version_t parsed;
        if (parseVersionString(payloadVersion, parsed) && parsed == version)
            return true;
    }
    return false;
}

bool deviceDatabase::getPayloadPath(const std::string &payloadType, const version_t *version, std::string &path,
                                    const std::string &role) const
{
    path.clear();
    if (familyPath_.empty())
        return false;

    std::ifstream fileStream(familyPath_.c_str());
    if (!fileStream.good())
        return false;

    json root;
    try { fileStream >> root; } catch (...) { return false; }

    std::string firstMatch;
    std::string defaultMatch;
    std::string latestMatch;

    for (const auto &payload : root.value("payloads", json::array()))
    {
        if (payload.value("type", std::string()) != payloadType)
            continue;

        // Disambiguates multiple same-type+version payloads that target
        // different MCUs within one multi-MCU device (e.g. KBP4's Central
        // vs Peripheral firmware - see kbp4.json's payload notes). role=""
        // (every existing caller, and every single-MCU family) only matches
        // role-less payloads, so this is a no-op everywhere except families
        // that actually tag a payload with a role.
        if (payload.value("role", std::string()) != role)
            continue;

        const std::string pathValue = payload.value("path", std::string());
        if (pathValue.empty())
            continue;

        // When a specific version is requested, only payloads matching that
        // version are eligible - but multiple payloads can share the same
        // version (e.g. a legacy monolithic .syx and a newer chunked
        // repackaging of the identical firmware release), so still prefer
        // latest/default among them rather than just the first array match.
        // Compares parsed version_t structs, not raw strings - see
        // isSupportedFirmwareVersion()'s comment for why a string compare is
        // unreliable across differing component counts.
        if (version != 0)
        {
            version_t payloadVersion;
            const std::string payloadVersionStr = payload.value("version", std::string());
            if (!parseVersionString(payloadVersionStr, payloadVersion) || !(payloadVersion == *version))
                continue;
        }

        if (firstMatch.empty())
            firstMatch = pathValue;
        if (payload.value("latest", false))
            latestMatch = pathValue;
        if (payload.value("default", false))
            defaultMatch = pathValue;
    }

    // Payload paths in the JSON are relative to the sendsysex repo root, same
    // as the family JSON itself - resolve them the same way rather than
    // handing back a path that only works when the caller's CWD happens to
    // already be the repo root.
    if (!latestMatch.empty())  { const std::string r = resolveDataPath(latestMatch);  path = r.empty() ? latestMatch  : r; return true; }
    if (!defaultMatch.empty()) { const std::string r = resolveDataPath(defaultMatch); path = r.empty() ? defaultMatch : r; return true; }
    if (!firstMatch.empty())   { const std::string r = resolveDataPath(firstMatch);   path = r.empty() ? firstMatch   : r; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// WinMM normalizer
//
// Formats seen on WinMM:
//   First port (bare alias): "QuNexus 3"          -> device port 1
//   Multi-port prefixed:     "MIDIIN2 (QuNexus) 4" -> device port 2
//   Bootloader etc.:         "MIDIIN2 (QuNexus Bootloader) 4" -> bootloader
//
// The trailing OS-assigned index on bare-alias names is always discarded;
// it is not the device port number. MIDIIN/MIDIOUT prefix number IS the
// device port number.
// ---------------------------------------------------------------------------
std::string deviceDatabase::normalizeWinMM(const std::string &raw) const
{
    const std::string trimmed = collapseWhitespaceCopy(trimCopy(raw));
    const std::string lowered = toLowerCopy(trimmed);

    // Case 1: MIDIIN[N] (DeviceName) OsIndex  or  MIDIOUT[N] (DeviceName) OsIndex
    if ((lowered.find("midiin") == 0 || lowered.find("midiout") == 0) && trimmed.find('(') != std::string::npos)
    {
        // Extract device port index from the numeric suffix on MIDIIN/MIDIOUT.
        // MIDIIN = port 1 (same as the bare alias), MIDIIN2 = port 2, etc.
        const std::string::size_type prefixEnd = lowered.find_first_of("0123456789 (");
        int devicePortIndex = 1;
        if (prefixEnd != std::string::npos && std::isdigit(static_cast<unsigned char>(lowered[prefixEnd])))
            devicePortIndex = std::atoi(lowered.c_str() + prefixEnd);

        // Extract device name from inside parens.
        const std::string::size_type openParen = trimmed.find('(');
        const std::string::size_type closeParen = trimmed.rfind(')');
        if (openParen == std::string::npos || closeParen == std::string::npos || closeParen <= openParen)
            return toLowerCopy(trimmed);

        const std::string innerName = collapseWhitespaceCopy(trimCopy(trimmed.substr(openParen + 1, closeParen - openParen - 1)));
        const std::string innerLower = toLowerCopy(innerName);

        // If the inner name contains a named USB descriptor suffix, the canonical
        // form is just the inner name lowercased (e.g. "softstep control surface").
        // Otherwise we look up by (productNameLower, devicePortIndex).
        if (devicePortIndex <= 1)
            return lookupByIndex(innerLower, 1);

        return lookupByIndex(innerLower, devicePortIndex);
    }

    // Case 2: Bare alias — "DeviceName OsIndex" where the trailing token is purely numeric.
    // This is always device port 1; the OS index is discarded entirely.
    {
        const std::string::size_type lastSpace = trimmed.rfind(' ');
        if (lastSpace != std::string::npos)
        {
            const std::string tail = trimCopy(trimmed.substr(lastSpace + 1));
            bool allDigits = !tail.empty();
            for (std::size_t i = 0; i < tail.size(); ++i)
                if (!std::isdigit(static_cast<unsigned char>(tail[i])))
                    allDigits = false;

            if (allDigits)
            {
                const std::string deviceName = toLowerCopy(collapseWhitespaceCopy(trimCopy(trimmed.substr(0, lastSpace))));
                return lookupByIndex(deviceName, 1);
            }
        }
    }

    // Case 3: No suffix — plain device name, treat as port 1.
    return lookupByIndex(lowered, 1);
}

// ---------------------------------------------------------------------------
// WinUWP normalizer
//
// Same logical structure as WinMM but no trailing OS index on multi-port names:
//   "MIDIIN2 (QuNexus)"   -> device port 2
//   "QuNexus"             -> device port 1
// ---------------------------------------------------------------------------
std::string deviceDatabase::normalizeWinUWP(const std::string &raw) const
{
    const std::string trimmed = collapseWhitespaceCopy(trimCopy(raw));
    const std::string lowered = toLowerCopy(trimmed);

    // Case 1: MIDIIN[N] (DeviceName)  or  MIDIOUT[N] (DeviceName)
    if ((lowered.find("midiin") == 0 || lowered.find("midiout") == 0) && trimmed.find('(') != std::string::npos)
    {
        const std::string::size_type prefixEnd = lowered.find_first_of("0123456789 (");
        int devicePortIndex = 1;
        if (prefixEnd != std::string::npos && std::isdigit(static_cast<unsigned char>(lowered[prefixEnd])))
            devicePortIndex = std::atoi(lowered.c_str() + prefixEnd);

        const std::string::size_type openParen = trimmed.find('(');
        const std::string::size_type closeParen = trimmed.rfind(')');
        if (openParen == std::string::npos || closeParen == std::string::npos || closeParen <= openParen)
            return lowered;

        const std::string innerName = toLowerCopy(collapseWhitespaceCopy(trimCopy(trimmed.substr(openParen + 1, closeParen - openParen - 1))));
        return lookupByIndex(innerName, devicePortIndex);
    }

    // Case 2: Plain device name — port 1.
    return lookupByIndex(lowered, 1);
}

// ---------------------------------------------------------------------------
// CoreAudioMIDI normalizer (macOS) - also used for Windows MIDI Services (WMS)
//
// Named ports: "QuNexus Control Surface"  -> directly canonical when lowercased
// Unnamed:     "QuNexus Port 1"           -> lookup by index
// Spanish:     "QuNexus Puerto 1"         -> normalize "puerto"/"peurto" first
//
// WMS quirk: unlike real CoreMIDI, which suffixes *every* port of an unnamed
// multi-port device ("QuNexus Port 1", "QuNexus Port 2"), Windows MIDI
// Services leaves port 1 completely bare ("12Step") and only suffixes port 2+
// ("12Step 2") - confirmed on a real legacy 12 Step unit. A bare name with no
// trailing index therefore has two possible meanings: a fully-named canonical
// descriptor for a genuinely named port (self-contained, matches knownPorts_
// directly), or WMS's unsuffixed port 1 of an otherwise unnamed multi-port
// device. Try the direct match first; only fall back to the port-1 lookup if
// the bare name isn't already a registered canonical name.
// ---------------------------------------------------------------------------
std::string deviceDatabase::normalizeCoreAudioMIDI(const std::string &raw) const
{
    std::string work = collapseWhitespaceCopy(trimCopy(raw));

    // Normalize Spanish "Puerto" / "Peurto" to "Port".
    {
        std::string::size_type pos = std::string::npos;
        while ((pos = work.find("Puerto")) != std::string::npos)
            work.replace(pos, 6, "Port");
        while ((pos = work.find("puerto")) != std::string::npos)
            work.replace(pos, 6, "Port");
        while ((pos = work.find("Peurto")) != std::string::npos)
            work.replace(pos, 6, "Port");
        while ((pos = work.find("peurto")) != std::string::npos)
            work.replace(pos, 6, "Port");
    }

    // Check for trailing "Port N" or "MIDI N" pattern.
    const std::string workLower = toLowerCopy(work);
    {
        // Match "... Port N" or "... MIDI N"
        const std::string::size_type lastSpace = work.rfind(' ');
        if (lastSpace != std::string::npos)
        {
            const std::string tail = trimCopy(work.substr(lastSpace + 1));
            bool allDigits = !tail.empty();
            for (std::size_t i = 0; i < tail.size(); ++i)
                if (!std::isdigit(static_cast<unsigned char>(tail[i])))
                    allDigits = false;

            if (allDigits)
            {
                const int portIndex = std::atoi(tail.c_str());
                const std::string beforeIndex = collapseWhitespaceCopy(trimCopy(work.substr(0, lastSpace)));
                const std::string beforeLower = toLowerCopy(beforeIndex);

                // Strip trailing "Port" or "MIDI" keyword to get the device name.
                std::string deviceName;
                if (beforeLower.size() >= 4 && beforeLower.rfind("port") == beforeLower.size() - 4)
                    deviceName = toLowerCopy(collapseWhitespaceCopy(trimCopy(beforeIndex.substr(0, beforeIndex.size() - 4))));
                else if (beforeLower.size() >= 4 && beforeLower.rfind("midi") == beforeLower.size() - 4)
                    deviceName = toLowerCopy(collapseWhitespaceCopy(trimCopy(beforeIndex.substr(0, beforeIndex.size() - 4))));
                else
                    deviceName = beforeLower;

                return lookupByIndex(deviceName, portIndex);
            }
        }
    }

    // No trailing index. Prefer a direct match (named USB descriptor, or a
    // genuinely single-port device's bare product name). If that fails, this
    // may be WMS's unsuffixed port 1 of an unnamed multi-port device.
    const std::string bareLower = toLowerCopy(work);
    for (std::size_t i = 0; i < knownPorts_.size(); ++i)
        if (knownPorts_[i].normalizedName == bareLower)
            return bareLower;

    return lookupByIndex(bareLower, 1);
}

// ---------------------------------------------------------------------------
// ALSA normalizer (Linux)
//
// Format: "DeviceName:DeviceName [PortName|MIDI N] AlsaClient:AlsaPort"
// Strip ALSA address from right, strip device-name prefix from left of colon.
// ---------------------------------------------------------------------------
std::string deviceDatabase::normalizeAlsa(const std::string &raw) const
{
    std::string work = collapseWhitespaceCopy(trimCopy(raw));

    // Strip trailing ALSA address token (digits:digits).
    {
        const std::string::size_type lastSpace = work.rfind(' ');
        if (lastSpace != std::string::npos)
        {
            const std::string tail = trimCopy(work.substr(lastSpace + 1));
            if (isAlsaAddressToken(tail))
                work = collapseWhitespaceCopy(trimCopy(work.substr(0, lastSpace)));
        }
    }

    // If the remaining string has "DeviceName:DeviceName PortPart", extract PortPart.
    {
        const std::string::size_type colonPos = work.find(':');
        if (colonPos != std::string::npos)
        {
            const std::string right = collapseWhitespaceCopy(trimCopy(work.substr(colonPos + 1)));
            const std::string left = toLowerCopy(collapseWhitespaceCopy(trimCopy(work.substr(0, colonPos))));
            const std::string rightLower = toLowerCopy(right);

            // Right side typically starts with the device name again; strip it.
            if (rightLower.find(left) == 0)
            {
                const std::string portPart = collapseWhitespaceCopy(trimCopy(right.substr(left.size())));
                // portPart is now something like "MIDI 1" or "Control Surface"
                // Try to extract an index from "MIDI N".
                const std::string portPartLower = toLowerCopy(portPart);
                if (portPartLower.find("midi ") == 0)
                {
                    const std::string indexStr = trimCopy(portPartLower.substr(5));
                    bool allDigits = !indexStr.empty();
                    for (std::size_t i = 0; i < indexStr.size(); ++i)
                        if (!std::isdigit(static_cast<unsigned char>(indexStr[i])))
                            allDigits = false;
                    if (allDigits)
                        return lookupByIndex(left, std::atoi(indexStr.c_str()));
                }
                // Named port descriptor — canonicalize as "devicename portdescriptor".
                if (!portPart.empty())
                    return toLowerCopy(left + " " + portPart);
                return left;
            }
            // Right side does not start with left — use it as-is.
            return toLowerCopy(right);
        }
    }

    return toLowerCopy(work);
}

// ---------------------------------------------------------------------------
// Public dispatch entry point
// ---------------------------------------------------------------------------
std::string deviceDatabase::normalizePortName(const std::string &rawPortName) const
{
    switch (activeApi_)
    {
        case RtMidi::WINDOWS_MM:
            return normalizeWinMM(rawPortName);
        case RtMidi::WINDOWS_UWP:
            return normalizeWinUWP(rawPortName);
        case RtMidi::WINDOWS_MIDI_SERVICES:
            // WMS reports each port's actual Group Terminal Block descriptor
            // name (e.g. "SoftStep Control Surface", "SoftStep Expander") -
            // "ProductName PortName", no numeric OS-assigned suffix - the
            // same shape CoreMIDI uses on macOS, and unlike WinMM's
            // positional scheme ("SoftStep" for port 1, "MIDIIN2 (SoftStep)"
            // for port 2+). normalizeWinMM() cannot parse this shape.
            return normalizeCoreAudioMIDI(rawPortName);
        case RtMidi::MACOSX_CORE:
            return normalizeCoreAudioMIDI(rawPortName);
        case RtMidi::LINUX_ALSA:
        case RtMidi::UNIX_JACK:
            return normalizeAlsa(rawPortName);
        default:
            // Unknown / unspecified API: attempt WinMM-style parse first,
            // then fall through to a simple lowercase.
            return normalizeWinMM(rawPortName);
    }
}

deviceDatabase::PortDescriptor deviceDatabase::describePort(const std::string &rawPortName) const
{
    PortDescriptor descriptor;
    descriptor.normalizedName = normalizePortName(rawPortName);

    for (std::size_t i = 0; i < knownPorts_.size(); ++i)
    {
        if (knownPorts_[i].normalizedName == descriptor.normalizedName)
        {
            descriptor.matched = true;
            descriptor.bootloader = knownPorts_[i].bootloader;
            descriptor.role = knownPorts_[i].role;
            return descriptor;
        }
    }

    // Probe-only fallback: no knownPorts_ registered — match any port whose
    // raw name contains a family marker (case-insensitive substring).
    if (probeOnly_)
    {
        const std::string rawLower = toLowerCopy(rawPortName);
        for (std::size_t i = 0; i < familyMarkers_.size(); ++i)
        {
            const std::string markerLower = toLowerCopy(familyMarkers_[i]);
            if (rawLower.find(markerLower) != std::string::npos)
            {
                descriptor.matched = true;
                descriptor.role = "main";
                return descriptor;
            }
        }
    }

    return descriptor;
}

bool deviceDatabase::matchesFamily(const std::string &rawPortName) const
{
    return describePort(rawPortName).matched;
}

bool deviceDatabase::isBootloaderPort(const std::string &rawPortName) const
{
    return describePort(rawPortName).bootloader;
}

std::string deviceDatabase::chooseBestPort(const std::vector<std::string> &rawPortNames, bool bootloaderState) const
{
    if (rawPortNames.empty())
        return std::string();

    const std::vector<std::string> &allowedRoles = bootloaderState ? safeProbeBootloaderRoles_ : safeProbeApplicationRoles_;

    for (std::size_t i = 0; i < allowedRoles.size(); ++i)
    {
        for (std::size_t j = 0; j < rawPortNames.size(); ++j)
        {
            const PortDescriptor descriptor = describePort(rawPortNames[j]);
            if (descriptor.matched && descriptor.role == allowedRoles[i])
                return rawPortNames[j];
        }
    }

    if (bootloaderState)
        for (std::size_t i = 0; i < rawPortNames.size(); ++i)
        {
            const PortDescriptor descriptor = describePort(rawPortNames[i]);
            if (descriptor.matched && descriptor.bootloader)
                return rawPortNames[i];
        }

    for (std::size_t i = 0; i < rawPortNames.size(); ++i)
        if (describePort(rawPortNames[i]).matched)
            return rawPortNames[i];

    return std::string();
}

std::vector<std::string> deviceDatabase::getOrderedPorts(const std::vector<std::string> &rawPortNames, bool inputDirection) const
{
    std::vector<std::string> ordered = rawPortNames;
    std::stable_sort(ordered.begin(), ordered.end(), [&](const std::string &left, const std::string &right) {
        const PortDescriptor leftDescriptor = describePort(left);
        const PortDescriptor rightDescriptor = describePort(right);
        const int leftOrder = getRoleOrder(leftDescriptor.role, inputDirection);
        const int rightOrder = getRoleOrder(rightDescriptor.role, inputDirection);

        if (leftOrder != rightOrder)
            return leftOrder < rightOrder;

        if (leftDescriptor.normalizedName != rightDescriptor.normalizedName)
            return leftDescriptor.normalizedName < rightDescriptor.normalizedName;

        return left < right;
    });

    return ordered;
}

int deviceDatabase::getApplicationPidMsb() const
{
    return applicationPidMsb_;
}

int deviceDatabase::getBootloaderPidMsb() const
{
    return bootloaderPidMsb_;
}

void deviceDatabase::addKnownPort(const std::string &portName, const std::string &role, bool bootloader)
{
    if (portName.empty())
        return;

    for (std::size_t i = 0; i < knownPorts_.size(); ++i)
        if (knownPorts_[i].normalizedName == portName)
            return;

    KnownPort knownPort;
    knownPort.normalizedName = portName;
    knownPort.role = role.empty() ? std::string("main") : role;
    knownPort.bootloader = bootloader;
    knownPorts_.push_back(knownPort);
}

void deviceDatabase::addPortByIndex(const std::string &productStringLower, int devicePortIndex, const std::string &normalizedName)
{
    if (productStringLower.empty() || devicePortIndex < 1 || normalizedName.empty())
        return;

    for (std::size_t i = 0; i < portsByIndex_.size(); ++i)
        if (portsByIndex_[i].productStringLower == productStringLower && portsByIndex_[i].devicePortIndex == devicePortIndex)
            return;

    PortByIndex entry;
    entry.productStringLower = productStringLower;
    entry.devicePortIndex = devicePortIndex;
    entry.normalizedName = normalizedName;
    portsByIndex_.push_back(entry);
}

void deviceDatabase::setActiveApi(RtMidi::Api api)
{
    activeApi_ = api;
}

std::string deviceDatabase::lookupByIndex(const std::string &productStringLower, int devicePortIndex) const
{
    for (std::size_t i = 0; i < portsByIndex_.size(); ++i)
        if (portsByIndex_[i].productStringLower == productStringLower && portsByIndex_[i].devicePortIndex == devicePortIndex)
            return portsByIndex_[i].normalizedName;

    // Fallback: generate a generic unnamed port name.
    std::ostringstream oss;
    oss << productStringLower << " port " << devicePortIndex;
    return oss.str();
}

int deviceDatabase::getRoleOrder(const std::string &role, bool inputDirection) const
{
    const std::vector<std::string> &roleOrder = inputDirection ? inputRoleOrder_ : outputRoleOrder_;
    for (std::size_t i = 0; i < roleOrder.size(); ++i)
        if (roleOrder[i] == role)
            return static_cast<int>(i);

    return static_cast<int>(roleOrder.size()) + 100;
}

std::string deviceDatabase::resolveDataPath(const std::string &relativePath) const
{
    return ::resolveDataPath(relativePath);
}
