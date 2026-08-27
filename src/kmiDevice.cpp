#include "kmiDevice.h"

#include "deviceHelpers.h"
#include "mfgLookup.h"
#include "MIDI_bytestream_parser.hpp"
#include "MIDI_sysex.hpp"
#include "midiBackend.h"
#include "RtMidi.h"
#include "sysExChunking.h"
#include "chunkedSysExTransfer.h"

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
// formatDuration/printProgress moved to deviceHelpers.h (shared with
// chunkedSysExTransfer.h's sendChunkedFileToPort(), used by both -f and
// --fw-update) so there's one progress-bar implementation, not two drifting
// copies.

// True for the two ways a firmware send to a bootloader port can fail that
// are ambiguous rather than definitely real failures - both traced to the
// same real-hardware finding (2026-08-26, SoftStep): some bootloaders finish
// writing/verifying and reboot back into application mode within the same
// window we were waiting for a bootloader-port reply on, so the bootloader
// port we were talking to is simply gone by the time we notice:
//   - chunkedSysExTransfer.h's "no identity reply after the final chunk"
//     (see its errorMessage assignment) - the reboot happened while we were
//     still waiting for a reply on the (about to be stale) connection.
//   - kmiDevice::openTransferOutputByName()'s "Exact MIDI output port not
//     found" - the reboot already happened by the time a retry attempt (see
//     chunkedSysExTransfer.h's sendChunkedFileWithRetry, up to 3 attempts)
//     tried to reopen the now-nonexistent bootloader port.
// Distinct from every other failure these can report (file read errors, a
// mid-transfer chunk actually dropped, the port never existing in the first
// place). Callers use this to decide whether to fall through to the existing
// "wait for application to reconnect" confirmation instead of failing
// outright - that loop makes the real call: if the device genuinely comes
// back in application mode this was a success, and if it never does, it
// already reports the accurate failure.
bool isAmbiguousBootloaderPortLoss(const std::string &err)
{
    return err.find("(the final chunk)") != std::string::npos
        || err.find("Exact MIDI output port not found") != std::string::npos;
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
    syxRx_->setCB_rx_HostMessage(&kmiDevice::midiCppHostMessageCallback);

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

void kmiDevice::setPortNameOverride(const std::string &appPortName, const std::string &bootloaderPortName)
{
    portNameOverrideActive_ = !appPortName.empty();
    overrideAppPortName_ = appPortName;
    overrideBootloaderPortName_ = bootloaderPortName.empty() ? appPortName : bootloaderPortName;
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

    if (!portNameOverrideActive_ && !database_.isLoaded() && !database_.loadFamily(familyId_))
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
        RtMidiOut tempApiProbe(midiBackend::selectedApi());
        database_.setActiveApi(tempApiProbe.getCurrentApi());
    }

    if (portNameOverrideActive_)
    {
        // Bypass family-marker/product-string matching entirely: the device
        // (e.g. a K-Board behind a Mimic Hub) reports a port name with no
        // relationship to its own product string, so trust the caller's
        // explicit name(s) instead. Both the app and bootloader alias are
        // checked since either (or both, if they're the same string) may be
        // visible depending on what's currently attached and its state -
        // app/bootloader disambiguation itself still happens later via the
        // identity reply's PID MSB (handleIdentityStateUpdate()), not here.
        for (std::size_t i = 0; i < visibleInputPorts_.size(); ++i)
            if (visibleInputPorts_[i] == overrideAppPortName_ || visibleInputPorts_[i] == overrideBootloaderPortName_)
                matchedInputPorts_.push_back(visibleInputPorts_[i]);

        for (std::size_t i = 0; i < visibleOutputPorts_.size(); ++i)
            if (visibleOutputPorts_[i] == overrideAppPortName_ || visibleOutputPorts_[i] == overrideBootloaderPortName_)
                matchedOutputPorts_.push_back(visibleOutputPorts_[i]);
    }
    else
    {
        for (std::size_t i = 0; i < visibleInputPorts_.size(); ++i)
            if (database_.matchesFamily(visibleInputPorts_[i]))
                matchedInputPorts_.push_back(visibleInputPorts_[i]);

        for (std::size_t i = 0; i < visibleOutputPorts_.size(); ++i)
            if (database_.matchesFamily(visibleOutputPorts_[i]))
                matchedOutputPorts_.push_back(visibleOutputPorts_[i]);
    }

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
                                   unsigned int postDelayMs,
                                   unsigned int firstGapDelayMs,
                                   unsigned int firstChunkSize,
                                   unsigned int idReplyTimeoutMs,
                                   unsigned int idReplyResendAttempts)
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

    // Multi-MCU families (currently only KBP4) can register a second
    // firmware payload tagged role="peripheral" for an MCU that's only
    // reachable while the primary MCU is in application mode, relaying over
    // its own transport (KBP4: I2C) rather than by SysEx bootloader flash.
    // Absent for every other family, so this is a no-op there. There's no
    // way to independently version-check this payload against the device
    // (peripherals have no MIDI identity of their own), so it's resent
    // unconditionally whenever the primary firmware update is pending -
    // same assumption the family JSON already encodes by versioning both
    // payloads identically. See kbp4.json's payload notes.
    std::string peripheralPath;
    const bool hasPeripheralFirmware = database_.getPayloadPath("firmware", &requestedFwVersion_, peripheralPath, "peripheral");
    bool peripheralSent = false;

    std::cout << "Automatic firmware update mode for " << familyName << " version " << versionToString(requestedFwVersion_) << "\n";
    if (hasPeripheralFirmware)
        std::cout << "Loading \"" << peripheralPath << "\"\n";
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
            if (!sendPayloadFileToPort(firmwarePath, bootloaderPort, chunkSize, chunkDelayMs, "firmware", postDelayMs, firstGapDelayMs, firstChunkSize, getFirmwareUpdateDefaults().rebootsToAppOnFinalChunk, idReplyTimeoutMs, idReplyResendAttempts))
            {
                if (!isAmbiguousBootloaderPortLoss(lastError_))
                    return false;
                std::cout << "No reply to the post-transfer identity check on the bootloader port - the "
                             "device may have already finished and rebooted to application mode. "
                             "Confirming via the application port...\n";
            }

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

        // Peripheral firmware (when this family has one) must go out first,
        // while the primary MCU is still in application mode - the
        // bootloader-entry command below reboots it into a state where the
        // relay is no longer available.
        if (hasPeripheralFirmware && !peripheralSent)
        {
            if (!sendPayloadFileToPort(peripheralPath, applicationPort, chunkSize, chunkDelayMs, "peripheral firmware", postDelayMs, firstGapDelayMs, firstChunkSize, false, idReplyTimeoutMs, idReplyResendAttempts))
                return false;
            peripheralSent = true;
        }

        if (!sendPayloadFileToPort(bootloaderEntryPath, applicationPort, chunkSize, chunkDelayMs, "bootloader-entry", postDelayMs, 0, 0, false, idReplyTimeoutMs, idReplyResendAttempts))
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
            if (!sendPayloadFileToPort(firmwarePath, bootloaderPort, chunkSize, chunkDelayMs, "firmware", postDelayMs, firstGapDelayMs, firstChunkSize, getFirmwareUpdateDefaults().rebootsToAppOnFinalChunk, idReplyTimeoutMs, idReplyResendAttempts))
            {
                if (!isAmbiguousBootloaderPortLoss(lastError_))
                    return false;
                std::cout << "No reply to the post-transfer identity check on the bootloader port - the "
                             "device may have already finished and rebooted to application mode. "
                             "Confirming via the application port...\n";
            }

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

            // Catches the case where the device was already in bootloader
            // mode when this run started (e.g. resuming after a prior
            // interrupted attempt) - the peripheral send above never ran
            // since it requires application mode, so do it now that we're
            // confirmed back in application mode instead of silently
            // skipping it. In the normal case (device started in
            // application mode) this is a no-op: peripheralSent is already
            // true from the send above.
            if (hasPeripheralFirmware && !peripheralSent)
            {
                const std::string applicationPort = activeOutputPortName_;
                disconnect();
                if (!sendPayloadFileToPort(peripheralPath, applicationPort, chunkSize, chunkDelayMs, "peripheral firmware", postDelayMs, firstGapDelayMs, firstChunkSize, false, idReplyTimeoutMs, idReplyResendAttempts))
                    return false;
                peripheralSent = true;
            }

            // Reconnecting in application mode is necessary but not
            // sufficient - it only proves *some* application firmware is
            // running, not that it's the version we just sent (a rejected/
            // corrupt flash write could fall back to whatever was there
            // before, if this bootloader supports that, or the update may
            // simply not have landed despite the device coming back up).
            // ensureCommsPortsForState() (called via refreshPorts() above)
            // always re-opens the port and re-requests identity fresh when
            // the active port name changes, so identityMetadata_ here
            // reflects a live read, not stale pre-update data.
            //
            // Deliberately NOT using firmwareUpdatePending_ for this - bug
            // found 2026-08-26 on real SoftStep hardware: --fw-update always
            // calls setFwVersion()/setDefaultFwVersion() with forceUpdate =
            // true (SendSysEx.cpp), and firmwareUpdatePending_ is defined as
            // forceFirmwareUpdate_ || (version mismatch) - so with force
            // always true, that flag is unconditionally true forever,
            // regardless of whether the device's version actually now
            // matches. Using it here made this check permanently fail even
            // on a fully successful update (confirmed: device correctly
            // reported 2.0.7 matching a requested 2.0.7, and this still
            // reported a mismatch before the fix). The version_t equality
            // operator already ignores the dev/build component by design
            // (MIDI_device_metadata.hpp), so a direct comparison is exactly
            // the "does the running version match" check needed here.
            // Exception (confirmByAppReconnectOnly): some families' application
            // firmware doesn't answer the standard Universal Device Inquiry at
            // all - e.g. MalletStation / EM Pro Riser (EM1 firmware) report
            // version only via a proprietary OSC-over-SysEx "/fw/w/version"
            // message, so an identity request in application mode never replies
            // and applicationVersion stays 0.0.0. For those a version match is
            // impossible; reconnecting on the application port (detected by port
            // name in refreshPorts(), not by an id reply) is the confirmation -
            // exactly what --fw-update used before the version-match check
            // existed. A device still stuck in the bootloader would show its
            // bootloader port here instead, so state_==connected already
            // distinguishes success from failure for these families.
            if (getFirmwareUpdateDefaults().confirmByAppReconnectOnly)
            {
                std::cout << "Confirmed application-mode reconnect (this family's application "
                             "firmware does not answer a standard identity request, so no version "
                             "match is performed).\n";
                return true;
            }

            if (identityMetadata_.applicationVersion != requestedFwVersion_)
            {
                lastError_ = "Device reconnected in application mode, but its reported firmware version ("
                           + versionToString(identityMetadata_.applicationVersion) + ") still does not match "
                             "the requested version (" + versionToString(requestedFwVersion_) + ") - the update "
                             "did not take effect.";
                return false;
            }

            std::cout << "Confirmed application version: " << versionToString(identityMetadata_.applicationVersion) << "\n";
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

const deviceDatabase::FirmwareUpdateDefaults &kmiDevice::getFirmwareUpdateDefaults() const
{
    return database_.getFirmwareUpdateDefaults();
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
        RtMidiIn tempIn(midiBackend::selectedApi());
        tempIn.ignoreTypes(false, false, false);
        const unsigned int numInPorts = tempIn.getPortCount();
        for (unsigned int i = 0; i < numInPorts; ++i)
            inputPorts.push_back(tempIn.getPortName(i));

        RtMidiOut tempOut(midiBackend::selectedApi());
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
    std::string desiredInput;
    std::string desiredOutput;

    if (portNameOverrideActive_)
    {
        // matchedInputPorts_/matchedOutputPorts_ were already filtered down to
        // just the override name(s) actually visible right now (refreshPorts());
        // there's nothing further to disambiguate by role here. App/bootloader
        // state itself comes from the identity reply's PID MSB, not from which
        // override name happened to match.
        if (!matchedInputPorts_.empty())
            desiredInput = matchedInputPorts_.front();
        if (!matchedOutputPorts_.empty())
            desiredOutput = matchedOutputPorts_.front();
    }
    else
    {
        bool bootloaderByName = false;
        for (std::size_t i = 0; i < matchedInputPorts_.size() && !bootloaderByName; ++i)
            bootloaderByName = database_.isBootloaderPort(matchedInputPorts_[i]);

        for (std::size_t i = 0; i < matchedOutputPorts_.size() && !bootloaderByName; ++i)
            bootloaderByName = database_.isBootloaderPort(matchedOutputPorts_[i]);

        const bool bootloaderState = bootloaderByName || state_ == State::bootloader;
        desiredInput = database_.chooseBestPort(matchedInputPorts_, bootloaderState);
        desiredOutput = database_.chooseBestPort(matchedOutputPorts_, bootloaderState);
    }

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
        midiIn_ = new RtMidiIn(midiBackend::selectedApi());
        midiIn_->ignoreTypes(false, false, false);
        const unsigned int numPorts = midiIn_->getPortCount();
        // Override mode: match the literal reported name exactly.
        // database_.normalizePortName() is driven by the family's registered
        // product strings/index table (loadFamily()) - for a name with no
        // relationship to those (e.g. "Mimic Hub MIDI Port N"), several
        // distinct unregistered names can normalize to the same empty/
        // fallback string and appear to "match" each other. Only trust it for
        // names actually drawn from the family JSON.
        const std::string normalizedRequest = portNameOverrideActive_ ? std::string() : database_.normalizePortName(portName);

        for (unsigned int i = 0; i < numPorts; ++i)
        {
            const std::string candidate = midiIn_->getPortName(i);
            if (portNameOverrideActive_)
            {
                if (candidate != portName)
                    continue;
            }
            else if (database_.normalizePortName(candidate) != normalizedRequest)
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
        midiOut_ = new RtMidiOut(midiBackend::selectedApi());
        const unsigned int numPorts = midiOut_->getPortCount();
        const std::string normalizedRequest = portNameOverrideActive_ ? std::string() : database_.normalizePortName(portName);

        for (unsigned int i = 0; i < numPorts; ++i)
        {
            const std::string candidate = midiOut_->getPortName(i);
            if (portNameOverrideActive_)
            {
                if (candidate != portName)
                    continue;
            }
            else if (database_.normalizePortName(candidate) != normalizedRequest)
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
        transferOut_ = new RtMidiOut(midiBackend::selectedApi());
        const unsigned int numPorts = transferOut_->getPortCount();
        const std::string normalizedRequest = portNameOverrideActive_ ? std::string() : database_.normalizePortName(portName);

        for (unsigned int i = 0; i < numPorts; ++i)
        {
            const std::string candidate = transferOut_->getPortName(i);
            if (portNameOverrideActive_)
            {
                if (candidate != portName)
                    continue;
            }
            else if (database_.normalizePortName(candidate) != normalizedRequest)
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
                                      unsigned int postDelayMs,
                                      unsigned int firstGapDelayMs,
                                      unsigned int firstChunkSize,
                                      bool finalChunkRebootsToApp,
                                      unsigned int idReplyTimeoutMs,
                                      unsigned int idReplyResendAttempts)
{
    std::vector<unsigned char> bytes;
    if (!readBinaryFile(filePath, bytes, &lastError_))
        return false;

    if (bytes.size() > MAX_MIDI_SYSEX_SIZE)
    {
        lastError_ = "SysEx file exceeds the supported send buffer size.";
        return false;
    }

    std::cout << "Sending " << label << " to port: " << portName << "\n";

    // Retry loop (port-not-ready-after-reboot recovery: WinMM may reject the
    // first sendMessage on a freshly opened port immediately after a device
    // reboot (MMRESULT=1) even though midiOutOpen() itself reported success)
    // now lives in chunkedSysExTransfer.h, shared with SendSysEx.cpp's raw
    // send path. openPort here keeps this class's own normalized-name
    // port-resolution semantics (openTransferOutputByName/database_), unlike
    // raw send's exact-name resolution - only the retry shape is shared.
    const std::vector<SysExChunk> chunks = findSysExChunks(bytes);
    return sendChunkedFileWithRetry(
        portName, bytes, chunks, chunkSize, chunkDelayMs, postDelayMs,
        [this, &portName]() -> RtMidiOut * {
            return openTransferOutputByName(portName) ? transferOut_ : 0;
        },
        [this]() { closeTransferPort(); },
        lastError_, firstGapDelayMs, firstChunkSize, finalChunkRebootsToApp,
        idReplyTimeoutMs, idReplyResendAttempts);
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
    if (self->database_.getVersionEncoding() == "bcd16")
    {
        // Legacy QuNeo-era identity reply (QuNeo_Firmware Q/Q_Main.c): each
        // version is only TWO bytes, [patch (LSB), (major<<4)|minor BCD nibbles
        // (MSB)], sent LSB-first - so the bootloader and app versions pack into
        // 4 contiguous bytes where the standard reply carries 6 (bl_ver[3] +
        // app_ver[3]). Straight-reading those 6 struct bytes misaligns: the
        // app's own LSB (patch) lands in bl_ver[2] and only its MSB reaches
        // app_ver[0], so a standard parse of QuNeo's 1.2.31 comes out "18.0.0"
        // (0x12 read as decimal). Undo the packing here:
        //   wire[0..3] = [bootLSB, bootMSB, appLSB, appMSB]
        //             = [bl_ver[0], bl_ver[1], bl_ver[2], app_ver[0]]
        self->identityMetadata_.bootloaderVersion.major = reply->bl_ver[1] >> 4;
        self->identityMetadata_.bootloaderVersion.minor = reply->bl_ver[1] & 0x0F;
        self->identityMetadata_.bootloaderVersion.patch = reply->bl_ver[0];
        self->identityMetadata_.bootloaderVersion.dev = 0;
        self->identityMetadata_.applicationVersion.major = reply->app_ver[0] >> 4;
        self->identityMetadata_.applicationVersion.minor = reply->app_ver[0] & 0x0F;
        self->identityMetadata_.applicationVersion.patch = reply->bl_ver[2];
        self->identityMetadata_.applicationVersion.dev = 0;
    }
    else
    {
        self->identityMetadata_.bootloaderVersion.major = reply->bl_ver[0];
        self->identityMetadata_.bootloaderVersion.minor = reply->bl_ver[1];
        self->identityMetadata_.bootloaderVersion.patch = reply->bl_ver[2];
        self->identityMetadata_.bootloaderVersion.dev = 0;
        self->identityMetadata_.applicationVersion.major = reply->app_ver[0];
        self->identityMetadata_.applicationVersion.minor = reply->app_ver[1];
        self->identityMetadata_.applicationVersion.patch = reply->app_ver[2];
        self->identityMetadata_.applicationVersion.dev = 0;
    }
    self->identityMetadata_.bootloaderStateKnown = true;
    self->identityMetadata_.isBootloader = (self->identityMetadata_.productIdMsb == self->database_.getBootloaderPidMsb());
    self->pendingIdentityRequest_ = false;
    self->handleIdentityStateUpdate();
}

// KBP4's Central APPLICATION firmware detects a standard Universal
// Non-Realtime Device Inquiry (F0 7E 7F 06 01 F7 - see MIDI_sysex.c's
// sx_process(), CORE_SX_HEADER case) but does not reply with a
// standards-format Identity Reply (06 02). It dispatches to
// send_firmware_version() (preset_sysex.c) instead, which replies with a
// KMI-proprietary "editor message" (SYX_FORMAT_KBP4_EDITOR_MESSAGE, msg
// type SYX_FIRMWARE_VERSION_MSG) that midiCppIDReplyCallback() above never
// sees - confirmed on real hardware 2026-08-17 (--id-request kbp4 always
// timed out in application mode, worked fine in bootloader mode, which
// does reply with a real Identity Reply). CORE_SX_HOST_DATA's own decode
// (msg_type/data_val/int_val, the latter packed as 3x7bit) is shaped for a
// different, smaller class of KBP4 editor messages and can't carry this
// reply's 16-byte version payload, so this callback re-reads the full raw
// RX buffer directly instead of trusting dataVal/intVal.
//
// Byte layout and SYX_FIRMWARE_VERSION_MSG's value (16) come from
// k-board_pro_firmware's
// Firmware/Keil_release_dev/kbp4_shared_code/I2C_shared/i2c_kbp4_shared.h
// (enum SYX_MESSAGE_TYPE) and .../kbp4_central/code/Main/preset_sysex.c
// (send_firmware_version()) - keep in sync if either changes. Version byte
// order (major,minor,patch,dev, direct - no swapping) confirmed against
// .../bootloader/kbp4_bootloader_shared_code/Boot/app_shared.c's
// array_form[0..3] assignment.
void kmiDevice::midiCppHostMessageCallback(void *userData, uint8_t msgType, uint8_t /*dataVal*/, uint16_t /*intVal*/)
{
    if (userData == 0)
        return;

    kmiDevice *self = static_cast<kmiDevice *>(userData);

    const uint8_t kKbp4FirmwareVersionMsgType = 16;
    if (self->familyId_ != "kbp4" || msgType != kKbp4FirmwareVersionMsgType || self->syxRx_ == 0)
        return;

    const uint8_t *buf = self->syxRx_->getData();
    const std::size_t len = self->syxRx_->getSize();

    // buffer[6]=length, [7]=msg type tag (== msgType above), [8..11]=Central
    // bootloader version, [12..15]=Central application version. [16..23]
    // would be Peripheral boot/app version, but preset_sysex.c currently
    // just duplicates the Central bytes there ("not yet implemented") - not
    // parsed here since it isn't real data.
    if (buf == 0 || len < 16)
        return;

    self->identityMetadata_.received = true;
    self->identityMetadata_.bootloaderVersion.major = buf[8];
    self->identityMetadata_.bootloaderVersion.minor = buf[9];
    self->identityMetadata_.bootloaderVersion.patch = buf[10];
    self->identityMetadata_.bootloaderVersion.dev   = buf[11];
    self->identityMetadata_.applicationVersion.major = buf[12];
    self->identityMetadata_.applicationVersion.minor = buf[13];
    self->identityMetadata_.applicationVersion.patch = buf[14];
    self->identityMetadata_.applicationVersion.dev   = buf[15];
    // This reply carries no manufacturer/product/family ID fields (unlike
    // SYSEX_DEVICE_INQUIRY_REPLY) and no reliable bootloader-vs-application
    // signal of its own, so bootloaderStateKnown is deliberately left
    // false - handleIdentityStateUpdate() already falls back to port-name
    // matching in that case, which is correct here since this reply only
    // ever comes from the application, never the bootloader.
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

