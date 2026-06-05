#include "kmiDevice.h"

#include "deviceHelpers.h"
#include "mfgLookup.h"
#include "MIDI_bytestream_parser.hpp"
#include "MIDI_sysex.hpp"
#include "RtMidi.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace
{
const std::size_t MAX_MIDI_SYSEX_SIZE = 250000;

std::string formatDuration(double seconds)
{
    if (seconds < 0.0)
        seconds = 0.0;

    int totalSeconds = static_cast<int>(seconds + 0.5);
    int minutes = totalSeconds / 60;
    int secs = totalSeconds % 60;

    std::ostringstream buffer;
    buffer << std::setw(2) << std::setfill('0') << minutes
           << ":" << std::setw(2) << std::setfill('0') << secs;
    return buffer.str();
}

void printProgress(std::size_t bytesSent,
                   std::size_t totalBytes,
                   std::size_t currentPacketSize,
                   const std::chrono::steady_clock::time_point &startTime)
{
    const int barWidth = 30;
    double progress = totalBytes > 0 ? static_cast<double>(bytesSent) / static_cast<double>(totalBytes) : 1.0;
    if (progress > 1.0)
        progress = 1.0;

    int filled = static_cast<int>(progress * barWidth + 0.5);
    if (filled > barWidth)
        filled = barWidth;

    const std::size_t bytesRemaining = totalBytes > bytesSent ? totalBytes - bytesSent : 0;
    const double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count() / 1000.0;
    const double eta = (bytesSent > 0 && progress > 0.0) ? (elapsed / static_cast<double>(bytesSent)) * static_cast<double>(bytesRemaining) : 0.0;

    std::cout << '\r'
              << '[' << std::string(filled, '#') << std::string(barWidth - filled, '-') << "] "
              << "pkt " << std::setw(4) << currentPacketSize << " B  "
              << "sent " << std::setw(6) << bytesSent << '/' << std::setw(6) << totalBytes << " B  "
              << "left " << std::setw(6) << bytesRemaining << " B  "
              << "elapsed " << formatDuration(elapsed) << "  "
              << "eta " << formatDuration(eta)
              << std::flush;

    if (bytesRemaining == 0)
        std::cout << "\n";
}
}

kmiDevice::kmiDevice(const std::string &familyId)
    : familyId_(normalizeFamilyId(familyId)),
      familyDisplayHint_(familyId),
      database_(familyId_),
      familyPresent_(false),
      state_(State::disconnected),
      midiIn_(0),
      midiOut_(0),
      transferOut_(0),
      syxTx_(0),
      syxRx_(0),
      byteParser_(0),
      requestedFwVersionValid_(false),
      forceFirmwareUpdate_(false),
      firmwareUpdatePending_(false),
      pendingIdentityRequest_(false)
{
    syxTx_ = new SysExMessageTX();
    syxRx_ = new SysExMessageRX(0);
    byteParser_ = new MidiBytestreamParser(syxRx_);

    syxTx_->setCB_tx_Context(this);
    syxTx_->setCB_send(&kmiDevice::midiCppSendCallback);
    syxRx_->setCB_rx_Context(this);
    syxRx_->setCB_rx_IDReply(&kmiDevice::midiCppIDReplyCallback);

    clearIdentityMetadata();
    if (!database_.isLoaded())
        lastError_ = database_.getLastError();
}

kmiDevice::~kmiDevice()
{
    disconnect();

    delete byteParser_;
    byteParser_ = 0;
    delete syxRx_;
    syxRx_ = 0;
    delete syxTx_;
    syxTx_ = 0;
}

bool kmiDevice::refreshPorts()
{
    const std::string previousActiveOutput = activeOutputPortName_;
    const State previousState = state_;

    visibleInputPorts_.clear();
    visibleOutputPorts_.clear();
    matchedInputPorts_.clear();
    matchedOutputPorts_.clear();
    familyPresent_ = false;
    lastError_.clear();

    if (!database_.isLoaded() && !database_.loadFamily(familyId_))
    {
        // If the JSON file simply doesn't exist, fall back to probe-only mode:
        // match any port whose raw name contains the family id as a substring.
        if (database_.getLastError().find("Could not locate") != std::string::npos)
            database_.loadFromName(familyDisplayHint_);
        else
        {
            lastError_ = database_.getLastError();
            closeCommsPorts();
            state_ = State::disconnected;
            return false;
        }
    }

    if (!scanPorts(visibleInputPorts_, visibleOutputPorts_, &lastError_))
    {
        closeCommsPorts();
        state_ = State::disconnected;
        return false;
    }

    // Tell the database which backend is active so it can apply the correct
    // port-name normalization strategy.
    {
        RtMidiOut tempApiProbe;
        database_.setActiveApi(tempApiProbe.getCurrentApi());
    }

    for (std::size_t i = 0; i < visibleInputPorts_.size(); ++i)
        if (database_.matchesFamily(visibleInputPorts_[i]))
            matchedInputPorts_.push_back(visibleInputPorts_[i]);

    for (std::size_t i = 0; i < visibleOutputPorts_.size(); ++i)
        if (database_.matchesFamily(visibleOutputPorts_[i]))
            matchedOutputPorts_.push_back(visibleOutputPorts_[i]);

    familyPresent_ = !matchedInputPorts_.empty() || !matchedOutputPorts_.empty();
    if (!familyPresent_)
    {
        closeCommsPorts();
        state_ = State::disconnected;
        return false;
    }

    if (!ensureCommsPortsForState())
    {
        if (lastError_.empty())
            lastError_ = "Family detected, but no exact probe input/output pair could be opened.";
        return false;
    }

    if (identityMetadata_.received)
        handleIdentityStateUpdate();
    else
    {
        const bool bootloaderByName = database_.isBootloaderPort(activeInputPortName_) || database_.isBootloaderPort(activeOutputPortName_);
        state_ = bootloaderByName ? State::bootloader : State::connected;
    }

    if (state_ == State::connected && !activeOutputPortName_.empty())
        if (previousState != State::connected || previousActiveOutput != activeOutputPortName_)
            printPortTranslations();

    return true;
}

void kmiDevice::disconnect()
{
    closeTransferPort();
    closeCommsPorts();
    familyPresent_ = false;
    state_ = State::disconnected;
}

bool kmiDevice::setDefaultFwVersion(bool forceUpdate)
{
    if (!database_.isLoaded() && !database_.loadFamily(familyId_))
    {
        lastError_ = database_.getLastError();
        return false;
    }

    std::string payloadPath;
    if (!database_.getPayloadPath("firmware", 0, payloadPath))
    {
        lastError_ = "No default firmware payload found for '" + familyId_ + "'.";
        return false;
    }

    // Resolve the version string from firmwarePayloadVersions_ that maps to this path.
    // Re-open the JSON to read the version field of the matched payload.
    std::string defaultVersion;
    if (!database_.getDefaultFirmwareVersion(defaultVersion))
    {
        lastError_ = "Could not determine default firmware version for '" + familyId_ + "'.";
        return false;
    }

    version_t version;
    if (!parseVersionString(defaultVersion, version))
    {
        lastError_ = "Default firmware version string '" + defaultVersion + "' could not be parsed.";
        return false;
    }

    return setFwVersion(version, forceUpdate);
}

bool kmiDevice::setFwVersion(const version_t &version, bool forceUpdate)
{
    if (!database_.isLoaded() && !database_.loadFamily(familyId_))
    {
        lastError_ = database_.getLastError();
        requestedFwVersionValid_ = false;
        firmwareUpdatePending_ = false;
        return false;
    }

    requestedFwVersion_ = version;
    forceFirmwareUpdate_ = forceUpdate;
    requestedFwVersionValid_ = database_.isSupportedFirmwareVersion(version);
    firmwareUpdatePending_ = false;

    if (!requestedFwVersionValid_)
    {
        lastError_ = "Requested firmware version " + versionToString(version) + " was not found in the family JSON payload list for '" + familyId_ + "'.";
        return false;
    }

    lastError_.clear();
    firmwareUpdatePending_ = forceUpdate;

    if (midiIn_ != 0 && midiOut_ != 0)
    {
        if (identityMetadata_.received)
            handleIdentityStateUpdate();
        else if (!pendingIdentityRequest_)
            sendIdentityRequest();
    }

    return true;
}

bool kmiDevice::runAutomaticUpdate(unsigned int chunkSize, unsigned int chunkDelayMs,
                                   unsigned int pollIntervalSeconds,
                                   unsigned int postDelayMs)
{
    if (!requestedFwVersionValid_)
    {
        lastError_ = "Automatic update requested without a valid firmware version.";
        return false;
    }

    if (chunkSize < 1 || chunkSize > MAX_MIDI_SYSEX_SIZE)
    {
        lastError_ = "Invalid chunk size for automatic update.";
        return false;
    }

    if (pollIntervalSeconds == 0)
        pollIntervalSeconds = 1;

    std::string firmwarePath;
    if (!database_.getPayloadPath("firmware", &requestedFwVersion_, firmwarePath))
    {
        lastError_ = "Could not locate the requested firmware payload in the family JSON.";
        return false;
    }

    std::string bootloaderEntryPath;
    const bool hasBootloaderEntry = database_.getPayloadPath("bootloader_entry", 0, bootloaderEntryPath);
    const std::string familyName = database_.getDisplayName().empty() ? familyId_ : database_.getDisplayName();

    std::cout << "Automatic firmware update mode for " << familyName << " version " << versionToString(requestedFwVersion_) << "\n";
    if (hasBootloaderEntry)
        std::cout << "Loading \"" << bootloaderEntryPath << "\"\n";
    std::cout << "Loading \"" << firmwarePath << "\"\n";

    bool firmwareSent = false;

    while (!firmwareSent)
    {
        std::cout << "Waiting " << pollIntervalSeconds << " second(s) for " << familyName << " to connect...\n";
        std::this_thread::sleep_for(std::chrono::seconds(pollIntervalSeconds));

        refreshPorts();
        if (state_ == State::disconnected)
            continue;

        if (state_ == State::bootloader)
        {
            const std::string bootloaderPort = activeOutputPortName_;
            std::cout << "Detected bootloader port: " << bootloaderPort << "\n";
            disconnect();
            // WinMM: allow handles to fully release and the bootloader to finish
            // any in-flight data before we open a new output port.
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            if (!sendPayloadFileToPort(firmwarePath, bootloaderPort, chunkSize, chunkDelayMs, "firmware", postDelayMs))
                return false;

            firmwareSent = true;
            break;
        }

        std::cout << "Detected application port: " << activeOutputPortName_ << "\n";
        if (!firmwareUpdatePending_)
        {
            std::cout << "Requested firmware already matches the connected device.\n";
            return true;
        }

        if (!hasBootloaderEntry)
        {
            lastError_ = "No bootloader-entry payload is configured for this family.";
            return false;
        }

        const std::string applicationPort = activeOutputPortName_;
        disconnect();
        if (!sendPayloadFileToPort(bootloaderEntryPath, applicationPort, chunkSize, chunkDelayMs, "bootloader-entry", postDelayMs))
            return false;

        while (true)
        {
            std::cout << "Waiting " << pollIntervalSeconds << " second(s) for bootloader mode...\n";
            std::this_thread::sleep_for(std::chrono::seconds(pollIntervalSeconds));

            refreshPorts();
            if (state_ != State::bootloader)
                continue;

            const std::string bootloaderPort = activeOutputPortName_;
            std::cout << "Found bootloader port: " << bootloaderPort << "\n";
            disconnect();
            // WinMM: allow handles to fully release and the bootloader to finish
            // any in-flight data before we open a new output port.
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            if (!sendPayloadFileToPort(firmwarePath, bootloaderPort, chunkSize, chunkDelayMs, "firmware", postDelayMs))
                return false;

            firmwareSent = true;
            break;
        }
    }

    for (unsigned int attempt = 0; attempt < 60; ++attempt)
    {
        std::cout << "Waiting " << pollIntervalSeconds << " second(s) for the device to return to application mode...\n";
        std::this_thread::sleep_for(std::chrono::seconds(pollIntervalSeconds));

        refreshPorts();
        if (state_ == State::connected)
        {
            std::cout << "Found application port: " << activeOutputPortName_ << "\n";
            return true;
        }
    }

    lastError_ = "Firmware payload sent, but the application reconnect was not confirmed.";
    return false;
}

kmiDevice::State kmiDevice::getState() const
{
    return state_;
}

bool kmiDevice::isFirmwareUpdatePending() const
{
    return firmwareUpdatePending_;
}

bool kmiDevice::hasReceivedIdentity() const
{
    return identityMetadata_.received;
}

const std::string &kmiDevice::getActiveOutputPortName() const
{
    return activeOutputPortName_;
}

int kmiDevice::findOutputPortNumberByName(RtMidiOut &midiOut, const std::string &requestedName)
{
    const std::string normalizedRequest = toLowerCopy(collapseWhitespaceCopy(requestedName));
    const unsigned int numOutPorts = midiOut.getPortCount();

    for (unsigned int i = 0; i < numOutPorts; ++i)
    {
        const std::string candidate = midiOut.getPortName(i);
        if (candidate == requestedName)
            return static_cast<int>(i);

        if (toLowerCopy(collapseWhitespaceCopy(candidate)) == normalizedRequest)
            return static_cast<int>(i);
    }

    return -1;
}

bool kmiDevice::getPayloadPath(const std::string &payloadType, const version_t *version, std::string &path) const
{
    return database_.getPayloadPath(payloadType, version, path);
}

const std::string &kmiDevice::getLastError() const
{
    return lastError_;
}

bool kmiDevice::sendFileToOpenPort(RtMidiOut &midiOut,
                                   const std::string &portName,
                                   const std::string &filePath,
                                   unsigned int chunkSize,
                                   unsigned int chunkDelayMs,
                                   std::string &errorMessage,
                                   bool sendAsSingleMessage,
                                   unsigned int postDelayMs)
{
    std::vector<unsigned char> bytes;
    if (!readBinaryFile(filePath, bytes, &errorMessage))
        return false;

    if (bytes.size() > MAX_MIDI_SYSEX_SIZE)
    {
        errorMessage = "SysEx file exceeds the supported send buffer size.";
        return false;
    }

    if (chunkSize < 1)
    {
        errorMessage = "Invalid chunk size for SysEx send.";
        return false;
    }

    try
    {
        const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
        std::size_t bytesSent = 0;

        if (sendAsSingleMessage || bytes.size() <= chunkSize)
        {
            midiOut.sendMessage(&bytes);
            bytesSent = bytes.size();
            printProgress(bytesSent, bytes.size(), bytes.size(), startTime);
        }
        else
        {
            while (bytesSent < bytes.size())
            {
                const std::size_t sizeToSend = std::min<std::size_t>(chunkSize, bytes.size() - bytesSent);
                std::vector<unsigned char> chunk(bytes.begin() + bytesSent, bytes.begin() + bytesSent + sizeToSend);
                midiOut.sendMessage(&chunk);
                bytesSent += sizeToSend;
                printProgress(bytesSent, bytes.size(), sizeToSend, startTime);

                if (bytesSent < bytes.size() && chunkDelayMs > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(chunkDelayMs));
            }
        }

        std::cout << "Successfully sent \"" << filePath << "\" to MIDI OUT port: "
                  << portName << ", size: " << bytes.size() << " bytes.\n";

        /* Hold the port open briefly so the receiver's USB driver can drain any
         * final NAK'd packets before the port is closed.  Without this delay the
         * CoreMIDI IOUSBKit layer cancels pending transfers on closePort(), which
         * can silently drop the SysEx F7 terminator. */
        if (postDelayMs > 0U)
            std::this_thread::sleep_for(std::chrono::milliseconds(postDelayMs));

        return true;
    }
    catch (RtMidiError &error)
    {
        errorMessage = error.getMessage();
    }
    catch (std::exception &error)
    {
        errorMessage = error.what();
    }

    return false;
}

void kmiDevice::clearIdentityMetadata()
{
    identityMetadata_ = IdentityMetadata();
    pendingIdentityRequest_ = false;

    if (syxRx_ != 0)
    {
        syxRx_->clear();
        syxRx_->rx_init();
    }

    if (byteParser_ != 0)
        byteParser_->reset();
}

void kmiDevice::printPortTranslations() const
{
    const std::vector<std::string> orderedInputs = database_.getOrderedPorts(matchedInputPorts_, true);
    const std::vector<std::string> orderedOutputs = database_.getOrderedPorts(matchedOutputPorts_, false);

    std::cout << "Port translations for "
              << (database_.getDisplayName().empty() ? familyId_ : database_.getDisplayName())
              << ":\n";

    if (orderedInputs.empty() && orderedOutputs.empty())
    {
        std::cout << "No visible ports matched this family.\n";
        return;
    }

    for (std::size_t i = 0; i < orderedInputs.size(); ++i)
    {
        const deviceDatabase::PortDescriptor descriptor = database_.describePort(orderedInputs[i]);
        const std::vector<std::string>::const_iterator rawIndex = std::find(visibleInputPorts_.begin(), visibleInputPorts_.end(), orderedInputs[i]);
        const int rtMidiPortNumber = (rawIndex == visibleInputPorts_.end()) ? -1 : static_cast<int>(rawIndex - visibleInputPorts_.begin());

        std::cout << "Input";
        if (rtMidiPortNumber >= 0)
            std::cout << " (RtMidi port " << rtMidiPortNumber << ")";
        std::cout << ": " << orderedInputs[i] << " -> " << descriptor.normalizedName;
        if (!descriptor.role.empty())
            std::cout << " [" << descriptor.role << "]";
        std::cout << "\n";
    }

    for (std::size_t i = 0; i < orderedOutputs.size(); ++i)
    {
        const deviceDatabase::PortDescriptor descriptor = database_.describePort(orderedOutputs[i]);
        const std::vector<std::string>::const_iterator rawIndex = std::find(visibleOutputPorts_.begin(), visibleOutputPorts_.end(), orderedOutputs[i]);
        const int rtMidiPortNumber = (rawIndex == visibleOutputPorts_.end()) ? -1 : static_cast<int>(rawIndex - visibleOutputPorts_.begin());

        std::cout << "Output";
        if (rtMidiPortNumber >= 0)
            std::cout << " (RtMidi port " << rtMidiPortNumber << ")";
        std::cout << ": " << orderedOutputs[i] << " -> " << descriptor.normalizedName;
        if (!descriptor.role.empty())
            std::cout << " [" << descriptor.role << "]";
        std::cout << "\n";
    }
}

void kmiDevice::printIdentityMetadata() const
{
    const std::string csvPath = resolveDataPath("lib/syxMfg/Sysex ID Tables/MIDI Sysex MFG IDs.csv");
    const std::string mfgName = csvPath.empty()
        ? std::string()
        : lookupManufacturerName(csvPath,
                                 identityMetadata_.manufacturerId[0],
                                 identityMetadata_.manufacturerId[1],
                                 identityMetadata_.manufacturerId[2]);

    std::cout << "ID reply received:\n";
    std::cout << "  Manufacturer ID: "
              << static_cast<int>(identityMetadata_.manufacturerId[0]) << "."
              << static_cast<int>(identityMetadata_.manufacturerId[1]) << "."
              << static_cast<int>(identityMetadata_.manufacturerId[2]);
    if (!mfgName.empty())
        std::cout << "  (" << mfgName << ")";
    std::cout << "\n";
    std::cout << "  Family ID: "
              << static_cast<int>(identityMetadata_.familyId[0]) << "."
              << static_cast<int>(identityMetadata_.familyId[1]) << "\n";
    std::cout << "  Product ID: msb=" << static_cast<int>(identityMetadata_.productIdMsb)
              << " lsb=" << static_cast<int>(identityMetadata_.productIdLsb) << "\n";
    std::cout << "  Bootloader version: " << versionToString(identityMetadata_.bootloaderVersion) << "\n";
    std::cout << "  Application version: " << versionToString(identityMetadata_.applicationVersion) << "\n";
    std::cout << "  Bootloader state: " << (identityMetadata_.isBootloader ? "bootloader" : "application") << "\n";
}

bool kmiDevice::scanPorts(std::vector<std::string> &inputPorts,
                          std::vector<std::string> &outputPorts,
                          std::string *errorMessage) const
{
    try
    {
        RtMidiIn tempIn;
        tempIn.ignoreTypes(false, false, false);
        const unsigned int numInPorts = tempIn.getPortCount();
        for (unsigned int i = 0; i < numInPorts; ++i)
            inputPorts.push_back(tempIn.getPortName(i));

        RtMidiOut tempOut;
        const unsigned int numOutPorts = tempOut.getPortCount();
        for (unsigned int i = 0; i < numOutPorts; ++i)
            outputPorts.push_back(tempOut.getPortName(i));
    }
    catch (RtMidiError &error)
    {
        if (errorMessage != 0)
            *errorMessage = error.getMessage();
        return false;
    }
    catch (std::exception &error)
    {
        if (errorMessage != 0)
            *errorMessage = error.what();
        return false;
    }

    return true;
}

bool kmiDevice::ensureCommsPortsForState()
{
    bool bootloaderByName = false;
    for (std::size_t i = 0; i < matchedInputPorts_.size() && !bootloaderByName; ++i)
        bootloaderByName = database_.isBootloaderPort(matchedInputPorts_[i]);

    for (std::size_t i = 0; i < matchedOutputPorts_.size() && !bootloaderByName; ++i)
        bootloaderByName = database_.isBootloaderPort(matchedOutputPorts_[i]);

    const bool bootloaderState = bootloaderByName || state_ == State::bootloader;
    const std::string desiredInput = database_.chooseBestPort(matchedInputPorts_, bootloaderState);
    const std::string desiredOutput = database_.chooseBestPort(matchedOutputPorts_, bootloaderState);

    if (desiredInput.empty() || desiredOutput.empty())
    {
        closeCommsPorts();
        return false;
    }

    if (midiIn_ != 0 && midiOut_ != 0 && desiredInput == activeInputPortName_ && desiredOutput == activeOutputPortName_)
    {
        if (!identityMetadata_.received && !pendingIdentityRequest_)
            sendIdentityRequest();
        return true;
    }

    closeCommsPorts();
    clearIdentityMetadata();

    if (!openInputByName(desiredInput))
    {
        closeCommsPorts();
        return false;
    }

    if (!openOutputByName(desiredOutput))
    {
        closeCommsPorts();
        return false;
    }

    activeInputPortName_ = desiredInput;
    activeOutputPortName_ = desiredOutput;

    // Allow the USB/MIDI driver to settle before sending anything.
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    sendIdentityRequest();
    for (int i = 0; i < 20 && pendingIdentityRequest_; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));

    if (identityMetadata_.received)
    {
        // Let the device finish processing before we send any follow-up messages.
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        handleIdentityStateUpdate();
    }

    return true;
}

void kmiDevice::closeCommsPorts()
{
    if (midiIn_ != 0)
    {
        try
        {
            if (midiIn_->isPortOpen())
                midiIn_->closePort();
        }
        catch (...)
        {
        }

        delete midiIn_;
        midiIn_ = 0;
    }

    if (midiOut_ != 0)
    {
        try
        {
            if (midiOut_->isPortOpen())
                midiOut_->closePort();
        }
        catch (...)
        {
        }

        delete midiOut_;
        midiOut_ = 0;
    }

    activeInputPortName_.clear();
    activeOutputPortName_.clear();
}

void kmiDevice::closeTransferPort()
{
    if (transferOut_ == 0)
        return;

    try
    {
        if (transferOut_->isPortOpen())
            transferOut_->closePort();
    }
    catch (...)
    {
    }

    delete transferOut_;
    transferOut_ = 0;
}

bool kmiDevice::openInputByName(const std::string &portName)
{
    try
    {
        midiIn_ = new RtMidiIn();
        midiIn_->ignoreTypes(false, false, false);
        const unsigned int numPorts = midiIn_->getPortCount();
        const std::string normalizedRequest = database_.normalizePortName(portName);

        for (unsigned int i = 0; i < numPorts; ++i)
        {
            const std::string candidate = midiIn_->getPortName(i);
            if (database_.normalizePortName(candidate) != normalizedRequest)
                continue;

            midiIn_->openPort(i, "kmiDevice input");
            midiIn_->setCallback(&kmiDevice::midiInputCallback, this);
            midiIn_->ignoreTypes(false, false, false);
            return true;
        }
    }
    catch (RtMidiError &error)
    {
        lastError_ = error.getMessage();
    }
    catch (std::exception &error)
    {
        lastError_ = error.what();
    }

    if (midiIn_ != 0)
    {
        delete midiIn_;
        midiIn_ = 0;
    }
    return false;
}

bool kmiDevice::openOutputByName(const std::string &portName)
{
    try
    {
        midiOut_ = new RtMidiOut();
        const unsigned int numPorts = midiOut_->getPortCount();
        const std::string normalizedRequest = database_.normalizePortName(portName);

        for (unsigned int i = 0; i < numPorts; ++i)
        {
            const std::string candidate = midiOut_->getPortName(i);
            if (database_.normalizePortName(candidate) != normalizedRequest)
                continue;

            midiOut_->openPort(i, "kmiDevice output");
            return true;
        }
    }
    catch (RtMidiError &error)
    {
        lastError_ = error.getMessage();
    }
    catch (std::exception &error)
    {
        lastError_ = error.what();
    }

    if (midiOut_ != 0)
    {
        delete midiOut_;
        midiOut_ = 0;
    }
    return false;
}

bool kmiDevice::openTransferOutputByName(const std::string &portName)
{
    closeTransferPort();

    try
    {
        transferOut_ = new RtMidiOut();
        const unsigned int numPorts = transferOut_->getPortCount();
        const std::string normalizedRequest = database_.normalizePortName(portName);

        for (unsigned int i = 0; i < numPorts; ++i)
        {
            const std::string candidate = transferOut_->getPortName(i);
            if (database_.normalizePortName(candidate) != normalizedRequest)
                continue;

            transferOut_->openPort(i, "kmiDevice transfer");
            return true;
        }

        lastError_ = "Exact MIDI output port not found for normalized name: " + normalizedRequest;
    }
    catch (RtMidiError &error)
    {
        lastError_ = error.getMessage();
    }
    catch (std::exception &error)
    {
        lastError_ = error.what();
    }

    closeTransferPort();
    return false;
}

bool kmiDevice::sendIdentityRequest()
{
    if (midiOut_ == 0 || syxTx_ == 0)
        return false;

    std::cout << "Sending identity request to: " << activeOutputPortName_ << "\n";
    pendingIdentityRequest_ = true;
    syxTx_->sendSysExIDRequest();
    return true;
}

bool kmiDevice::sendPayloadFileToPort(const std::string &filePath,
                                      const std::string &portName,
                                      unsigned int chunkSize,
                                      unsigned int chunkDelayMs,
                                      const std::string &label,
                                      unsigned int postDelayMs)
{
    // On Windows, the WinMM driver may reject the first sendMessage on a
    // freshly opened port immediately after a device reboot (MMRESULT=1),
    // even though midiOutOpen() reported success.  Retry up to 3 times;
    // on each retry close the handle, wait for the driver to fully release
    // the device, then reopen.  This covers the common case where a device
    // reboots into bootloader mode and its USB transfer endpoint is not yet
    // ready to accept data.
    static const int kMaxAttempts = 3;
    static const int kRetryDelayMs = 3000;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
    {
        if (!openTransferOutputByName(portName))
            return false;

        // Give WinMM a moment to settle after any recently closed handles
        // before issuing the first sendMessage on the freshly opened port.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::cout << "Opening port: " << portName << "\n";
        std::cout << "Sending " << label << " to port: " << portName << "\n";

        const bool sent = kmiDevice::sendFileToOpenPort(*transferOut_,
                                                        portName,
                                                        filePath,
                                                        chunkSize,
                                                        chunkDelayMs,
                                                        lastError_,
                                                        label != "firmware",
                                                        postDelayMs);
        closeTransferPort();

        if (sent)
            return true;

        if (attempt + 1 < kMaxAttempts)
        {
            std::cout << "Port not ready, retrying in " << (kRetryDelayMs / 1000) << " second(s)...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
        }
    }

    return false;
}

void kmiDevice::processIncomingMessage(const std::vector<unsigned char> &message)
{
    if (byteParser_ == 0)
        return;

    for (std::size_t i = 0; i < message.size(); ++i)
        byteParser_->parse(message[i]);
}

void kmiDevice::handleIdentityStateUpdate()
{
    if (!identityMetadata_.received)
        return;

    if (requestedFwVersionValid_)
        firmwareUpdatePending_ = forceFirmwareUpdate_ || (identityMetadata_.applicationVersion != requestedFwVersion_);
    else
        firmwareUpdatePending_ = forceFirmwareUpdate_;

    if (!identityMetadata_.bootloaderStateKnown)
    {
        const bool bootloaderByName = database_.isBootloaderPort(activeInputPortName_) || database_.isBootloaderPort(activeOutputPortName_);
        state_ = bootloaderByName ? State::bootloader : State::connected;
        return;
    }

    state_ = identityMetadata_.isBootloader ? State::bootloader : State::connected;
}

int16_t kmiDevice::midiCppSendCallback(void *userData, uint8_t *data, uint16_t length)
{
    if (userData == 0 || data == 0 || length == 0)
        return -1;

    kmiDevice *self = static_cast<kmiDevice *>(userData);
    if (self->midiOut_ == 0)
    {
        self->pendingIdentityRequest_ = false;
        return -1;
    }

    try
    {
        std::vector<unsigned char> message(data, data + length);
        self->midiOut_->sendMessage(&message);
        return 0;
    }
    catch (RtMidiError &error)
    {
        self->lastError_ = error.getMessage();
    }
    catch (std::exception &error)
    {
        self->lastError_ = error.what();
    }

    self->pendingIdentityRequest_ = false;
    return -1;
}

void kmiDevice::midiCppIDReplyCallback(void *userData, SYSEX_DEVICE_INQUIRY_REPLY *reply)
{
    if (userData == 0 || reply == 0)
        return;

    kmiDevice *self = static_cast<kmiDevice *>(userData);
    self->identityMetadata_.received = true;
    self->identityMetadata_.manufacturerId[0] = reply->metadata.mfg_id[0];
    self->identityMetadata_.manufacturerId[1] = reply->metadata.mfg_id[1];
    self->identityMetadata_.manufacturerId[2] = reply->metadata.mfg_id[2];
    self->identityMetadata_.productIdLsb = reply->metadata.prod_id[0];
    self->identityMetadata_.productIdMsb = reply->metadata.prod_id[1];
    self->identityMetadata_.familyId[0] = reply->metadata.family_id[0];
    self->identityMetadata_.familyId[1] = reply->metadata.family_id[1];
    self->identityMetadata_.bootloaderVersion.major = reply->bl_ver[0];
    self->identityMetadata_.bootloaderVersion.minor = reply->bl_ver[1];
    self->identityMetadata_.bootloaderVersion.patch = reply->bl_ver[2];
    self->identityMetadata_.bootloaderVersion.dev = 0;
    self->identityMetadata_.applicationVersion.major = reply->app_ver[0];
    self->identityMetadata_.applicationVersion.minor = reply->app_ver[1];
    self->identityMetadata_.applicationVersion.patch = reply->app_ver[2];
    self->identityMetadata_.applicationVersion.dev = 0;
    self->identityMetadata_.bootloaderStateKnown = true;
    self->identityMetadata_.isBootloader = (self->identityMetadata_.productIdMsb == self->database_.getBootloaderPidMsb());
    self->pendingIdentityRequest_ = false;
    self->handleIdentityStateUpdate();
}

void kmiDevice::midiInputCallback(double, std::vector<unsigned char> *message, void *userData)
{
    if (message == 0 || userData == 0)
        return;

    kmiDevice *self = static_cast<kmiDevice *>(userData);
    self->processIncomingMessage(*message);
}

