#ifndef KMI_DEVICE_H
#define KMI_DEVICE_H

#include <cstdint>
#include <string>
#include <vector>

#include "MIDI_device_metadata.hpp"
#include "deviceDatabase.h"

namespace rt { namespace midi { class RtMidiIn; class RtMidiOut; } }
using rt::midi::RtMidiIn;
using rt::midi::RtMidiOut;
class SysExMessageTX;
class SysExMessageRX;
class MidiBytestreamParser;

class kmiDevice
{
public:
    struct IdentityMetadata
    {
        bool received = false;
        bool bootloaderStateKnown = false;
        bool isBootloader = false;
        uint8_t manufacturerId[3] = {0, 0, 0};
        uint8_t familyId[2] = {0, 0};
        uint8_t productIdLsb = 0;
        uint8_t productIdMsb = 0;
        version_t bootloaderVersion = {0, 0, 0, 0};
        version_t applicationVersion = {0, 0, 0, 0};
    };

    enum class State
    {
        disconnected,
        connected,
        bootloader
    };

    explicit kmiDevice(const std::string &familyId);
    ~kmiDevice();

    // Overrides port discovery entirely for devices whose reported MIDI port
    // name has no relationship to the family's product-string/role table --
    // e.g. a K-Board behind a Mimic Hub, which always reports a generic
    // "Mimic Hub MIDI Port N" name regardless of what's attached or its
    // app/bootloader state. When set, refreshPorts() skips family-marker
    // matching and looks for these exact literal names instead; bootloader
    // vs application selection still comes from the identity reply's PID MSB
    // (unaffected by this override; see handleIdentityStateUpdate()), not
    // from which override name matched. If bootloaderPortName is empty, it
    // defaults to appPortName (the common case: a device like K-Board that
    // reports the same port name in both states even on a direct connection).
    void setPortNameOverride(const std::string &appPortName, const std::string &bootloaderPortName = std::string());

    bool refreshPorts();
    void disconnect();
    bool setFwVersion(const version_t &version, bool forceUpdate);
    bool setDefaultFwVersion(bool forceUpdate);
    bool runAutomaticUpdate(unsigned int chunkSize, unsigned int chunkDelayMs,
                            unsigned int pollIntervalSeconds,
                            unsigned int postDelayMs = 500U,
                            unsigned int firstGapDelayMs = 0,
                            unsigned int firstChunkSize = 0);

    State getState() const;
    bool isFirmwareUpdatePending() const;
    bool hasReceivedIdentity() const;
    const std::string &getActiveOutputPortName() const;
    bool getPayloadPath(const std::string &payloadType, const version_t *version, std::string &path) const;
    const deviceDatabase::FirmwareUpdateDefaults &getFirmwareUpdateDefaults() const;
    const std::string &getLastError() const;
    void printPortTranslations() const;
    void printIdentityMetadata() const;
    static int findOutputPortNumberByName(RtMidiOut &midiOut, const std::string &requestedName);

    static bool sendFileToOpenPort(RtMidiOut &midiOut,
                                   const std::string &portName,
                                   const std::string &filePath,
                                   unsigned int chunkSize,
                                   unsigned int chunkDelayMs,
                                   std::string &errorMessage,
                                   bool sendAsSingleMessage = false,
                                   unsigned int postDelayMs = 500U);

private:
    bool scanPorts(std::vector<std::string> &inputPorts,
                   std::vector<std::string> &outputPorts,
                   std::string *errorMessage) const;
    bool ensureCommsPortsForState();
    void closeCommsPorts();
    void closeTransferPort();

    bool openInputByName(const std::string &portName);
    bool openOutputByName(const std::string &portName);
    bool openTransferOutputByName(const std::string &portName);
    bool sendIdentityRequest();
    bool sendPayloadFileToPort(const std::string &filePath,
                               const std::string &portName,
                               unsigned int chunkSize,
                               unsigned int chunkDelayMs,
                               const std::string &label,
                               unsigned int postDelayMs = 500U,
                               unsigned int firstGapDelayMs = 0,
                               unsigned int firstChunkSize = 0,
                               bool finalChunkRebootsToApp = false);
    void processIncomingMessage(const std::vector<unsigned char> &message);
    void handleIdentityStateUpdate();
    void clearIdentityMetadata();

    static int16_t midiCppSendCallback(void *userData, uint8_t *data, uint16_t length);
    static void midiCppIDReplyCallback(void *userData, SYSEX_DEVICE_INQUIRY_REPLY *reply);
    static void midiCppHostMessageCallback(void *userData, uint8_t msgType, uint8_t dataVal, uint16_t intVal);
    static void midiInputCallback(double timeStamp, std::vector<unsigned char> *message, void *userData);

    std::string familyId_;
    std::string familyDisplayHint_;
    deviceDatabase database_;
    bool familyPresent_;
    State state_;

    RtMidiIn *midiIn_;
    RtMidiOut *midiOut_;
    RtMidiOut *transferOut_;
    SysExMessageTX *syxTx_;
    SysExMessageRX *syxRx_;
    MidiBytestreamParser *byteParser_;

    std::vector<std::string> visibleInputPorts_;
    std::vector<std::string> visibleOutputPorts_;
    std::vector<std::string> matchedInputPorts_;
    std::vector<std::string> matchedOutputPorts_;

    std::string activeInputPortName_;
    std::string activeOutputPortName_;
    bool portNameOverrideActive_ = false;
    std::string overrideAppPortName_;
    std::string overrideBootloaderPortName_;
    std::string lastError_;
    IdentityMetadata identityMetadata_;
    version_t requestedFwVersion_ = {0, 0, 0, 0};
    bool requestedFwVersionValid_;
    bool forceFirmwareUpdate_;
    bool firmwareUpdatePending_;
    bool pendingIdentityRequest_;
};

#endif
