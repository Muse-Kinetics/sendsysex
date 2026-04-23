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

    bool refreshPorts();
    void disconnect();
    bool setFwVersion(const version_t &version, bool forceUpdate);
    bool setDefaultFwVersion(bool forceUpdate);
    bool runAutomaticUpdate(unsigned int chunkSize, unsigned int chunkDelayMs, unsigned int pollIntervalSeconds);

    State getState() const;
    bool isFirmwareUpdatePending() const;
    bool hasReceivedIdentity() const;
    const std::string &getActiveOutputPortName() const;
    bool getPayloadPath(const std::string &payloadType, const version_t *version, std::string &path) const;
    const std::string &getLastError() const;

    //! True when the last refreshPorts() call failed because the Windows MIDI
    //! Services SDK runtime is not installed on this machine.
    bool requiresSdkInstall() const;

    //! The installer download URL when requiresSdkInstall() is true, otherwise empty.
    const std::string &getSdkInstallUrl() const;
    void printPortTranslations() const;
    void printIdentityMetadata() const;
    static int findOutputPortNumberByName(RtMidiOut &midiOut, const std::string &requestedName);

    static bool sendFileToOpenPort(RtMidiOut &midiOut,
                                   const std::string &portName,
                                   const std::string &filePath,
                                   unsigned int chunkSize,
                                   unsigned int chunkDelayMs,
                                   std::string &errorMessage,
                                   bool sendAsSingleMessage = false);

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
                               const std::string &label);
    void processIncomingMessage(const std::vector<unsigned char> &message);
    void handleIdentityStateUpdate();
    void clearIdentityMetadata();

    static int16_t midiCppSendCallback(void *userData, uint8_t *data, uint16_t length);
    static void midiCppIDReplyCallback(void *userData, SYSEX_DEVICE_INQUIRY_REPLY *reply);
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
    std::string lastError_;
    bool sdkInstallRequired_ = false;
    std::string sdkInstallUrl_;
    IdentityMetadata identityMetadata_;
    version_t requestedFwVersion_ = {0, 0, 0, 0};
    bool requestedFwVersionValid_;
    bool forceFirmwareUpdate_;
    bool firmwareUpdatePending_;
    bool pendingIdentityRequest_;
};

#endif
