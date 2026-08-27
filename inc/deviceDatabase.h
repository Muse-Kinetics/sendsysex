#ifndef DEVICE_DATABASE_H
#define DEVICE_DATABASE_H

#include <string>
#include <vector>

#include "MIDI_device_metadata.hpp"
#include "RtMidi.h"

class deviceDatabase
{
public:
    struct PortDescriptor
    {
        bool matched = false;
        bool bootloader = false;
        std::string role;
        std::string normalizedName;
    };

    // Mirrors data/schemas/kmi_family.schema.json's transport.firmwareUpdateDefaults.
    // All-zero (the default) means the family JSON has no such block, i.e. --fw-update
    // should behave exactly as it did before this field existed for this family.
    struct FirmwareUpdateDefaults
    {
        unsigned int firstChunkSize = 0;
        unsigned int firstGapDelayMs = 0;
        unsigned int chunkDelayMs = 0;
        unsigned int postDelayMs = 0;
        // Per-attempt identity-reply wait budget, and how many additional
        // times to resend the request and wait again if a chunk stays
        // silent, before the chunk (and then the whole transfer) is
        // considered failed. 0 for either means "family JSON doesn't
        // override this," matching the all-zero-default convention above -
        // the CLI's own --id-reply-timeout/--id-reply-resend-attempts
        // defaults apply instead. See chunkedSysExTransfer.h's 2026-08-25
        // redesign notes.
        unsigned int idReplyTimeoutMs = 0;
        unsigned int idReplyResendAttempts = 0;
        // When true, the device reboots straight into application mode as it
        // commits the final firmware chunk, so it never answers an identity
        // request on the bootloader port afterwards. The chunked sender skips
        // that doomed final-chunk handshake and lets runAutomaticUpdate's
        // application-port reconnect + version check be the success test (see
        // isAmbiguousBootloaderPortLoss). Default false = the historical
        // behavior (wait postDelayMs for a final bootloader reply).
        bool rebootsToAppOnFinalChunk = false;
        // When true, confirm a successful update by application-mode reconnect
        // alone, skipping the post-update version match. For families whose
        // application firmware does not answer a standard Universal Device
        // Inquiry (e.g. MalletStation / EM Pro Riser, which report version only
        // via a proprietary OSC-over-SysEx message), a version match is
        // impossible - see kmiDevice::runAutomaticUpdate. Default false keeps
        // the strict version-match confirmation.
        bool confirmByAppReconnectOnly = false;
    };

    deviceDatabase();
    explicit deviceDatabase(const std::string &familyId);

    bool loadFamily(const std::string &familyId);
    bool loadFromName(const std::string &displayName);
    bool isLoaded() const;

    void setActiveApi(RtMidi::Api api);

    const std::string &getFamilyId() const;
    const std::string &getDisplayName() const;
    const std::string &getLastError() const;

    bool isSupportedFirmwareVersion(const version_t &version) const;
    bool getPayloadPath(const std::string &payloadType, const version_t *version, std::string &path,
                        const std::string &role = std::string()) const;
    bool getDefaultFirmwareVersion(std::string &version) const;
    const FirmwareUpdateDefaults &getFirmwareUpdateDefaults() const;

    std::string normalizePortName(const std::string &rawPortName) const;
    PortDescriptor describePort(const std::string &rawPortName) const;
    bool matchesFamily(const std::string &rawPortName) const;
    bool isBootloaderPort(const std::string &rawPortName) const;
    std::string chooseBestPort(const std::vector<std::string> &rawPortNames, bool bootloaderState) const;
    std::vector<std::string> getOrderedPorts(const std::vector<std::string> &rawPortNames, bool inputDirection) const;

    int getApplicationPidMsb() const;
    int getBootloaderPidMsb() const;

    // Identity-reply version byte layout for this family. "standard" (default):
    // each version is 3 bytes read straight as major/minor/patch. "bcd16":
    // legacy QuNeo-era layout - each version is only 2 bytes in the reply,
    // [patch(LSB), (major<<4)|minor (BCD nibbles, MSB)], so the two versions
    // pack into 4 bytes total (see kmiDevice::midiCppIDReplyCallback).
    const std::string &getVersionEncoding() const;

private:
    struct KnownPort
    {
        std::string normalizedName;
        std::string role;
        bool bootloader;
    };

    // Secondary index keyed by (toLower(productString), 1-based device port index)
    struct PortByIndex
    {
        std::string productStringLower;
        int         devicePortIndex;
        std::string normalizedName; // canonical result stored in knownPorts_
    };

    void clear();
    void addKnownPort(const std::string &portName, const std::string &role, bool bootloader);
    void addPortByIndex(const std::string &productStringLower, int devicePortIndex, const std::string &normalizedName);
    int getRoleOrder(const std::string &role, bool inputDirection) const;
    std::string resolveDataPath(const std::string &relativePath) const;

    // Per-backend raw name parsers; each returns a canonical normalized name.
    std::string normalizeWinMM(const std::string &raw) const;
    std::string normalizeWinUWP(const std::string &raw) const;
    std::string normalizeCoreAudioMIDI(const std::string &raw) const;
    std::string normalizeAlsa(const std::string &raw) const;

    // Resolve a (productStringLower, 1-based devicePortIndex) pair to a canonical name.
    // Falls back to productStringLower + " port " + index if the index entry is unknown.
    std::string lookupByIndex(const std::string &productStringLower, int devicePortIndex) const;

    std::string familyId_;
    std::string displayName_;
    std::string familyPath_;
    std::string lastError_;
    bool loaded_;
    bool probeOnly_;
    int applicationPidMsb_;
    int bootloaderPidMsb_;
    std::string versionEncoding_;
    RtMidi::Api activeApi_;
    std::vector<std::string> familyMarkers_;
    std::vector<std::string> safeProbeApplicationRoles_;
    std::vector<std::string> safeProbeBootloaderRoles_;
    std::vector<std::string> inputRoleOrder_;
    std::vector<std::string> outputRoleOrder_;
    std::vector<std::string> firmwarePayloadVersions_;
    std::vector<KnownPort> knownPorts_;
    std::vector<PortByIndex> portsByIndex_;
    FirmwareUpdateDefaults firmwareUpdateDefaults_;
};

#endif
