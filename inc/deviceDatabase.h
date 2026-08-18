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

    std::string normalizePortName(const std::string &rawPortName) const;
    PortDescriptor describePort(const std::string &rawPortName) const;
    bool matchesFamily(const std::string &rawPortName) const;
    bool isBootloaderPort(const std::string &rawPortName) const;
    std::string chooseBestPort(const std::vector<std::string> &rawPortNames, bool bootloaderState) const;
    std::vector<std::string> getOrderedPorts(const std::vector<std::string> &rawPortNames, bool inputDirection) const;

    int getApplicationPidMsb() const;
    int getBootloaderPidMsb() const;

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
    RtMidi::Api activeApi_;
    std::vector<std::string> familyMarkers_;
    std::vector<std::string> safeProbeApplicationRoles_;
    std::vector<std::string> safeProbeBootloaderRoles_;
    std::vector<std::string> inputRoleOrder_;
    std::vector<std::string> outputRoleOrder_;
    std::vector<std::string> firmwarePayloadVersions_;
    std::vector<KnownPort> knownPorts_;
    std::vector<PortByIndex> portsByIndex_;
};

#endif
