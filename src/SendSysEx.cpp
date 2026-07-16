//
//  SendSysEx.cpp
//
//  Written by Eric Bateman on 2023/3/31
//  SPDX-License-Identifier: MIT

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "RtMidi.h"
#include "deviceHelpers.h"
#include "kmiDevice.h"

namespace
{
const std::size_t MAX_MIDI_SYSEX_SIZE = 250000;
const unsigned int DEFAULT_CHUNK_SIZE = 48;
const unsigned int DEFAULT_CHUNK_DELAY_MS = 2;
const unsigned int DEFAULT_POLL_SECONDS = 1;

struct CliOptions
{
    bool listPortsOnly = false;
    bool listNormalizedOnly = false;
    bool usePortNumber = false;
    bool automaticMode = false;
    bool idRequestOnly = false;
    bool showHelp = false;
    int portNumber = -1;
    std::string portName;
    std::string filePath;
    std::string familyName;
    std::string versionText;
    std::string parseError;
    unsigned int chunkSize = DEFAULT_CHUNK_SIZE;
    unsigned int chunkDelayMs = DEFAULT_CHUNK_DELAY_MS;
    unsigned int pollSeconds = DEFAULT_POLL_SECONDS;
    unsigned int postDelayMs = 500U;
    bool blEraseRebootCmd = false;
    int blPid = -1;
};


void printHelp()
{
    std::cout
        << "\nSend SysEx Utility"
        << "\n(c) 2026 KMI Music, Inc."
        << "\nAuthor: Eric Bateman (eric@musekinetics.com)\n"
        << "\nModes:\n"
        << "  1. Raw send mode: Send a SysEx file directly to a raw RtMidi output port.\n"
        << "  2. Automatic update mode: Hand off to kmiDevice for family-based firmware updates.\n"
        << "\nUsage:\n"
        << "  SendSysEx -l\n"
        << "  SendSysEx --list-normalized [family]\n"
        << "  SendSysEx -p <port-number> -f <file.syx> [-cs <bytes>] [-cd <ms>] [-pd <ms>]\n"
        << "  SendSysEx -n <raw-port-name> -f <file.syx> [-cs <bytes>] [-cd <ms>] [-pd <ms>]\n"
        << "  SendSysEx --fw-update <family> [--fw-version <version>] [-t <seconds>] [-cs <bytes>] [-cd <ms>] [-pd <ms>]\n"
        << "  SendSysEx --id-request <family> [-t <seconds>]\n"
        << "\nRaw send mode:\n"
        << "  -l                         List all raw RtMidi input and output ports\n"
        << "  --list-normalized [family] List raw RtMidi port names and their normalized forms\n"
        << "                             Defaults to SoftStep if no family is provided\n"
        << "  -p <num>                   Select a raw RtMidi output port by number\n"
        << "  -n <name>                  Select a raw RtMidi output port by exact name\n"
        << "  -f <file>                  SysEx file to send directly to the selected raw port\n"
        << "\nAutomatic update mode:\n"
        << "  --fw-update <family>       Supported family name such as SoftStep, QuNeo, or QuNexus\n"
        << "  --fw-version <version>     Firmware version; omit to use the default from the family JSON\n"
        << "  --id-request <family>      Connect, send an identity request, print the reply, then exit\n"
        << "  -t <seconds>               Poll interval while waiting for the device to appear or reboot\n"
        << "\nTransfer options:\n"
        << "  -cs, --chunk-size <bytes>   SysEx chunk size in bytes (default: 48)\n"
        << "  -cd, --chunk-delay <ms>     Delay between chunks in milliseconds (default: 2)\n"
        << "  -pd, --post-delay <ms>      Wait N ms after last chunk before closing port (default: 500)\n"
        << "                              Prevents F7 loss when the receiver NAKs the final packet.\n"
        << "                              Use 0 to disable.\n"
        << "  -h, --help                  Show this help message\n"
        << "\nNotes:\n"
        << "  - Raw send mode uses RtMidi directly and does not normalize port names for you.\n"
        << "  - Automatic update mode uses the family database and normalized exact matching internally.\n"
        << "  - Raw send mode (-p/-n -f) auto-detects files containing multiple independently-\n"
        << "    framed F0...F7 SysEx messages (Hex_to_SysEx's chunked output). Each such chunk is\n"
        << "    sent whole (unless -cs is smaller than the chunk, in which case that chunk alone is\n"
        << "    sub-split into -cs-byte windows). Between chunks, -cd becomes the identity-reply\n"
        << "    handshake timeout: an ID request is sent and must be answered within -cd ms before\n"
        << "    the next chunk goes out, aborting the transfer otherwise. Single-message (legacy)\n"
        << "    files are sent as before, with no handshake.\n"
        << "\nExamples:\n"
        << "  SendSysEx -l\n"
        << "  SendSysEx --list-normalized QuNexus\n"
        << "  SendSysEx -p 0 -f firmware.syx\n"
        << "  SendSysEx -n \"SoftStep Bootloader 1\" -f firmware.syx\n"
        << "  SendSysEx --fw-update SoftStep\n"
        << "  SendSysEx --fw-update SoftStep --fw-version 2.0.4\n"
        << "  SendSysEx --id-request QuNexus\n"
        << "  SendSysEx --send-bl-erase-reboot-cmd --pid <pid>\n\n"
        << "Bootloader commands:\n"
        << "  --send-bl-erase-reboot-cmd  Send a double-EOF erase+reboot command to a KMI bootloader.\n"
        << "                              Requires --pid <pid> (decimal or 0x-prefixed hex).\n"
        << "                              Requires -p or -n to select the MIDI output port.\n\n";
}

void listPorts(RtMidiOut &midiOut)
{
    RtMidiIn midiIn;
    const unsigned int numInPorts = midiIn.getPortCount();
    for (unsigned int i = 0; i < numInPorts; ++i)
        std::cout << "MIDI Input  Port " << i << ": " << midiIn.getPortName(i) << "\n";

    const unsigned int numOutPorts = midiOut.getPortCount();
    for (unsigned int i = 0; i < numOutPorts; ++i)
        std::cout << "MIDI Output Port " << i << ": " << midiOut.getPortName(i) << "\n";
}

void listNormalizedPorts(const std::string &familyName)
{
    const std::string familyId = familyName.empty() ? std::string("softstep") : normalizeFamilyId(familyName);
    kmiDevice device(familyId);
    const bool refreshed = device.refreshPorts();

    if (!refreshed && !device.getLastError().empty())
        std::cout << "Note: " << device.getLastError() << "\n";

    if (!refreshed || device.getState() != kmiDevice::State::connected)
        device.printPortTranslations();
}

// A single complete SysEx message (0xF0 ... 0xF7 inclusive) found within a file.
struct SysExChunk
{
    std::size_t offset;
    std::size_t length;
};

// Scans a SysEx file for one or more back-to-back, independently-framed
// F0...F7 messages ("chunks"), as produced by Hex_to_SysEx's -cs chunked mode
// (see Hex_to_SysEx/README.md). Payload bytes inside a message are always
// 7-bit-encoded (top bit clear per the KMI encode/decode scheme), so a raw
// 0xF0/0xF7 byte anywhere in the file can only be real framing - no risk of
// mistaking encoded data for a boundary marker. A file produced the old way
// (one giant KMI-formatted payload) simply comes back as a single chunk.
std::vector<SysExChunk> findSysExChunks(const std::vector<unsigned char> &bytes)
{
    std::vector<SysExChunk> chunks;
    std::size_t i = 0;
    while (i < bytes.size())
    {
        if (bytes[i] != 0xF0)
        {
            ++i;
            continue;
        }
        const std::size_t start = i;
        std::size_t j = i + 1;
        while (j < bytes.size() && bytes[j] != 0xF7)
            ++j;
        if (j >= bytes.size())
            break; // trailing, unterminated F0 - not a complete message, ignore it

        chunks.push_back(SysExChunk{start, j - start + 1});
        i = j + 1;
    }
    return chunks;
}

// WinMM assigns a trailing disambiguation index to bare-alias port names
// independently per port-direction list, so the same physical device can
// enumerate as e.g. input "K-Board 4" / output "K-Board 5" - same device,
// different OS-assigned suffix. Strip a trailing " <digits>" token so
// input/output names for the same device compare equal.
std::string stripTrailingIndex(const std::string &name)
{
    const std::string::size_type lastSpace = name.find_last_of(' ');
    if (lastSpace == std::string::npos)
        return name;

    const std::string tail = name.substr(lastSpace + 1);
    if (tail.empty())
        return name;

    for (std::string::const_iterator it = tail.begin(); it != tail.end(); ++it)
        if (!std::isdigit(static_cast<unsigned char>(*it)))
            return name;

    return name.substr(0, lastSpace);
}

int findMatchingInputPort(RtMidiIn &midiIn, const std::string &outputPortName)
{
    const std::string outNorm = stripTrailingIndex(outputPortName);
    const unsigned int n = midiIn.getPortCount();
    for (unsigned int i = 0; i < n; ++i)
    {
        try
        {
            const std::string inName = midiIn.getPortName(i);
            if (inName == outputPortName || stripTrailingIndex(inName) == outNorm)
                return static_cast<int>(i);
        }
        catch (...)
        {
        }
    }
    return -1;
}

struct ReplyState
{
    volatile bool received;
    ReplyState() : received(false) {}
};

void chunkReplyCallback(double /*timeStamp*/, std::vector<unsigned char> *message, void *userData)
{
    ReplyState *state = static_cast<ReplyState *>(userData);
    if (message != 0 && !message->empty() && (*message)[0] == 0xF0)
        state->received = true;
}

// Sends a Universal Non-Realtime Device Inquiry (F0 7E 7F 06 01 F7) and waits
// up to waitMs for any SysEx reply (delivered via chunkReplyCallback).
bool waitForIdReply(RtMidiOut &midiOut, ReplyState &state, unsigned int waitMs)
{
    static const std::vector<unsigned char> idRequest = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    state.received = false;
    midiOut.sendMessage(&idRequest);

    const unsigned int pollIntervalMs = 5;
    unsigned int waited = 0;
    while (!state.received && waited < waitMs)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
        waited += pollIntervalMs;
    }
    return state.received;
}

// Sends every detected chunk in order. For files with more than one chunk, an
// identity request is sent between chunks and must be answered within
// chunkDelayMs before the next chunk is sent - the reliability strategy
// sendsysex/docs/kmi_fw_update_process.md calls out ("optionally send SysEx
// identity requests between chunks and wait for reply before continuing"),
// which the K-Board bootloader supports: each chunk is independently framed
// and CRC-validated, while the underlying hex-record parser state persists
// across chunks (K-Board_Bootloader/code/MIDI/MIDI_sysex.c - static locals in
// firmware_packet_process() survive across separate F0...F7 messages). A
// single-chunk file (the historical one-giant-message format) just sends with
// no handshake, same as before.
//
// chunkSize only matters when it's smaller than an individual chunk's length,
// in which case that chunk is sub-split into chunkSize-byte windows sent
// back-to-back (still the same message's bytes, delivered across multiple
// host-side writes - fine, since order is preserved). When chunkSize is
// already >= the chunk's length (the normal case for a properly chunked
// file), each chunk goes out as a single, whole, correctly-framed message.
bool sendChunkedFileToPort(RtMidiOut &midiOut,
                           const std::string &outputPortName,
                           const std::vector<unsigned char> &bytes,
                           const std::vector<SysExChunk> &chunks,
                           unsigned int chunkSize,
                           unsigned int chunkDelayMs,
                           unsigned int postDelayMs,
                           std::string &errorMessage)
{
    if (chunks.empty())
    {
        errorMessage = "No complete SysEx message (0xF0...0xF7) found in file.";
        return false;
    }

    if (chunks.size() > 1)
    {
        std::size_t minLen = chunks.front().length;
        std::size_t maxLen = chunks.front().length;
        for (std::size_t i = 1; i < chunks.size(); ++i)
        {
            minLen = std::min(minLen, chunks[i].length);
            maxLen = std::max(maxLen, chunks[i].length);
        }
        std::cout << "Chunked SysEx file detected: " << chunks.size() << " chunk(s), "
                  << minLen << "-" << maxLen << " bytes each (" << bytes.size()
                  << " bytes total)\n";
    }

    // Only the multi-chunk case needs the identity-request handshake, so only
    // it needs an input port opened.
    RtMidiIn midiIn;
    ReplyState replyState;
    bool haveInput = false;
    if (chunks.size() > 1)
    {
        midiIn.ignoreTypes(false, false, false);
        const int inputPortNumber = findMatchingInputPort(midiIn, outputPortName);
        if (inputPortNumber < 0)
        {
            errorMessage = "Could not find a MIDI input port matching \"" + outputPortName
                          + "\" for the between-chunk identity handshake.";
            return false;
        }
        midiIn.setCallback(chunkReplyCallback, &replyState);
        midiIn.openPort(static_cast<unsigned int>(inputPortNumber));
        haveInput = true;
    }

    bool ok = true;
    for (std::size_t i = 0; i < chunks.size() && ok; ++i)
    {
        const SysExChunk &chunk = chunks[i];
        std::cout << "Chunk " << (i + 1) << "/" << chunks.size()
                  << " (" << chunk.length << " bytes)... ";
        std::cout.flush();

        try
        {
            if (chunkSize < chunk.length)
            {
                std::cout << "(sub-split into " << chunkSize << "-byte windows, -cs < chunk size) ";
                std::size_t sent = 0;
                while (sent < chunk.length)
                {
                    const std::size_t sizeToSend = std::min<std::size_t>(chunkSize, chunk.length - sent);
                    std::vector<unsigned char> window(bytes.begin() + chunk.offset + sent,
                                                       bytes.begin() + chunk.offset + sent + sizeToSend);
                    midiOut.sendMessage(&window);
                    sent += sizeToSend;
                }
            }
            else
            {
                std::vector<unsigned char> whole(bytes.begin() + chunk.offset,
                                                 bytes.begin() + chunk.offset + chunk.length);
                midiOut.sendMessage(&whole);
            }
        }
        catch (RtMidiError &error)
        {
            errorMessage = error.getMessage();
            ok = false;
            break;
        }

        const bool isLastChunk = (i + 1 == chunks.size());
        if (!isLastChunk)
        {
            if (waitForIdReply(midiOut, replyState, chunkDelayMs))
            {
                std::cout << "sent, reply OK (" << (chunks.size() - i - 1) << " chunk(s) remaining)\n";
            }
            else
            {
                std::cout << "sent, NO REPLY within " << chunkDelayMs << " ms\n";
                errorMessage = "No identity reply after chunk " + std::to_string(i + 1) + "/"
                              + std::to_string(chunks.size()) + " - aborting transfer.";
                ok = false;
            }
        }
        else
        {
            std::cout << "sent (last chunk)\n";
        }
    }

    if (haveInput)
        midiIn.closePort();

    if (ok)
    {
        std::cout << "Successfully sent all " << chunks.size() << " chunk(s) to MIDI OUT port: "
                  << outputPortName << ", size: " << bytes.size() << " bytes.\n";
        if (postDelayMs > 0U)
            std::this_thread::sleep_for(std::chrono::milliseconds(postDelayMs));
    }

    return ok;
}

bool sendFileToPort(RtMidiOut &midiOut,
                    int portNumber,
                    const std::string &filePath,
                    unsigned int chunkSize,
                    unsigned int chunkDelayMs,
                    unsigned int postDelayMs,
                    std::string &errorMessage)
{
    try
    {
        const std::string portName = midiOut.getPortName(static_cast<unsigned int>(portNumber));
        std::cout << "Opening port: " << portNumber << "\n";
        midiOut.openPort(static_cast<unsigned int>(portNumber));

        std::vector<unsigned char> bytes;
        if (!readBinaryFile(filePath, bytes, &errorMessage))
        {
            midiOut.closePort();
            return false;
        }

        const std::vector<SysExChunk> chunks = findSysExChunks(bytes);
        const bool sent = sendChunkedFileToPort(midiOut, portName, bytes, chunks,
                                                chunkSize, chunkDelayMs, postDelayMs, errorMessage);
        midiOut.closePort();
        return sent;
    }
    catch (RtMidiError &error)
    {
        errorMessage = error.getMessage();
    }
    catch (std::exception &error)
    {
        errorMessage = error.what();
    }

    try
    {
        if (midiOut.isPortOpen())
            midiOut.closePort();
    }
    catch (...)
    {
    }

    return false;
}

CliOptions parseArguments(int argc, const char *argv[])
{
    CliOptions options;

    if (argc < 2)
    {
        options.showHelp = true;
        return options;
    }

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];

        if (arg == "-h" || arg == "--help")
        {
            options.showHelp = true;
            return options;
        }
        if (arg == "-l")
        {
            options.listPortsOnly = true;
            continue;
        }
        if (arg == "--list-normalized")
        {
            options.listNormalizedOnly = true;
            continue;
        }
        if (arg == "-p")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for -p.";
                return options;
            }

            unsigned int parsedPortNumber = 0;
            if (!parseUnsignedOption(argv[++i], parsedPortNumber))
            {
                options.parseError = "Invalid port number.";
                return options;
            }

            options.usePortNumber = true;
            options.portNumber = static_cast<int>(parsedPortNumber);
            continue;
        }
        if (arg == "-n")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for -n.";
                return options;
            }

            options.portName = argv[++i];
            continue;
        }
        if (arg == "-f")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for -f.";
                return options;
            }

            options.filePath = argv[++i];
            continue;
        }
        if (arg == "-t")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for -t.";
                return options;
            }

            unsigned int pollSeconds = DEFAULT_POLL_SECONDS;
            if (!parseUnsignedOption(argv[++i], pollSeconds) || pollSeconds == 0)
            {
                options.parseError = "Invalid poll interval.";
                return options;
            }
            options.pollSeconds = pollSeconds;
            continue;
        }
        if (arg == "-cs" || arg == "--chunk-size")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for --chunk-size.";
                return options;
            }

            if (!parseUnsignedOption(argv[++i], options.chunkSize) || options.chunkSize < 1 || options.chunkSize > MAX_MIDI_SYSEX_SIZE)
            {
                options.parseError = "Invalid chunk size. Use a value from 1 to " + std::to_string(MAX_MIDI_SYSEX_SIZE) + " bytes.";
                return options;
            }
            continue;
        }
        if (arg == "-cd" || arg == "--chunk-delay")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for --chunk-delay.";
                return options;
            }

            if (!parseUnsignedOption(argv[++i], options.chunkDelayMs))
            {
                options.parseError = "Invalid chunk delay.";
                return options;
            }
            continue;
        }
        if (arg == "-pd" || arg == "--post-delay")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for --post-delay.";
                return options;
            }

            if (!parseUnsignedOption(argv[++i], options.postDelayMs))
            {
                options.parseError = "Invalid post delay.";
                return options;
            }
            continue;
        }
        if (arg == "--fw-update")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for --fw-update.";
                return options;
            }

            options.automaticMode = true;
            options.familyName = argv[++i];
            continue;
        }
        if (arg == "--id-request")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for --id-request.";
                return options;
            }

            options.idRequestOnly = true;
            options.familyName = argv[++i];
            continue;
        }
        if (options.listNormalizedOnly && options.familyName.empty() && !options.automaticMode && !arg.empty() && arg[0] != '-')
        {
            options.familyName = arg;
            continue;
        }
        if (arg.find("--fw-update=") == 0)
        {
            options.automaticMode = true;
            options.familyName = arg.substr(std::string("--fw-update=").size());
            continue;
        }
        if (arg == "--fw-version")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for --fw-version.";
                return options;
            }

            options.automaticMode = true;
            options.versionText = argv[++i];
            continue;
        }
        if (arg.find("--fw-version=") == 0)
        {
            options.automaticMode = true;
            options.versionText = arg.substr(std::string("--fw-version=").size());
            continue;
        }
        if (arg == "--send-bl-erase-reboot-cmd")
        {
            options.blEraseRebootCmd = true;
            continue;
        }
        if (arg == "--pid")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for --pid.";
                return options;
            }
            const std::string pidStr = argv[++i];
            unsigned long parsed = 0;
            try
            {
                std::size_t pos = 0;
                parsed = std::stoul(pidStr, &pos, 0); // base 0: auto-detect 0x prefix
                if (pos != pidStr.size() || parsed > 255)
                    throw std::invalid_argument("");
            }
            catch (...)
            {
                options.parseError = "Invalid PID '" + pidStr + "'. Use a decimal or 0x-prefixed hex value 0-255.";
                return options;
            }
            options.blPid = static_cast<int>(parsed);
            continue;
        }

        options.showHelp = true;
        options.parseError = "Unrecognized argument: " + arg;
        return options;
    }

    return options;
}

int runIdentityRequest(const CliOptions &options)
{
    if (options.familyName.empty())
    {
        std::cout << "ERROR: --id-request requires a family name.\n";
        return 1;
    }

    kmiDevice device(options.familyName);

    std::cout << "Waiting for " << options.familyName << " to appear...\n";

    for (unsigned int attempt = 0; attempt < 60; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::seconds(options.pollSeconds));

        if (device.refreshPorts())
            break;

        std::cout << "Waiting " << options.pollSeconds << " second(s) for " << options.familyName << " to connect...\n";
    }

    if (device.getState() == kmiDevice::State::disconnected)
    {
        std::cout << "ERROR: " << (device.getLastError().empty() ? "Device not found." : device.getLastError()) << "\n";
        return 1;
    }

    // refreshPorts already sends the identity request and waits up to 500 ms.
    // If the reply has not arrived yet (slow device), poll a bit longer.
    if (!device.hasReceivedIdentity())
    {
        std::cout << "Waiting for identity reply...\n";
        for (int i = 0; i < 40 && !device.hasReceivedIdentity(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!device.hasReceivedIdentity())
    {
        std::cout << "No identity reply received within the timeout.\n";
        return 1;
    }

    device.printIdentityMetadata();
    return 0;
}

int runAutomaticProcess(const CliOptions &options)
{
    if (options.familyName.empty())
    {
        std::cout << "ERROR: --fw-update requires a family name.\n";
        return 1;
    }

    kmiDevice device(options.familyName);

    if (options.versionText.empty())
    {
        if (!device.setDefaultFwVersion(true))
        {
            std::cout << "ERROR: " << device.getLastError() << "\n";
            return 1;
        }
    }
    else
    {
        version_t requestedVersion;
        if (!parseVersionString(options.versionText, requestedVersion))
        {
            std::cout << "ERROR: Invalid firmware version format. Use a value such as 2.0.4.\n";
            return 1;
        }
        if (!device.setFwVersion(requestedVersion, true))
        {
            std::cout << "ERROR: " << device.getLastError() << "\n";
            return 1;
        }
    }

    if (!device.runAutomaticUpdate(options.chunkSize, options.chunkDelayMs, options.pollSeconds, options.postDelayMs))
    {
        std::cout << "ERROR: " << device.getLastError() << "\n";
        return 1;
    }

    std::cout << "Automatic update flow finished.\n";
    return 0;
}

int runBlEraseRebootCmd(const CliOptions &options)
{
    if (options.blPid < 0)
    {
        std::cout << "ERROR: --send-bl-erase-reboot-cmd requires --pid <pid>.\n";
        return 1;
    }
    if (!options.usePortNumber && options.portName.empty())
    {
        std::cout << "ERROR: --send-bl-erase-reboot-cmd requires -p <num> or -n <name>.\n";
        return 1;
    }

    const unsigned char pid = static_cast<unsigned char>(options.blPid);

    // Double-EOF erase+reboot payload for KMI C8051F38x bootloader.
    // Encoding: KMI SysEx header + 4 zero pads + packet-start 0x01 +
    // encoded preamble [START_OF_TEXT=0x0002, FIRMWARE_PACKET id=0x1110, preamble-CRC] +
    // two encoded Intel HEX EOF records [SX_HEX_LINE_START, totalLen=7, 0x3A, 0x00,
    // 0x00, 0x00, type=1, hex-crc=0xFF, line-CRC=0x6440] + 0xF7.
    const std::vector<unsigned char> payload = {
        0xF0, 0x00, 0x01, 0x5F, 0x7A, pid,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x02, 0x11, 0x10, 0x48, 0x53, 0x00, 0x30,
        0x03, 0x07, 0x3A, 0x00, 0x00, 0x00, 0x01, 0x00, 0x7F, 0x64, 0x40,
        0x00, 0x00, 0x00, 0x00, 0x01,
        0x03, 0x07, 0x3A, 0x00, 0x00, 0x00, 0x01, 0x00, 0x7F, 0x64, 0x40,
        0x00, 0x00, 0x00, 0x00, 0x01,
        0xF7
    };

    try
    {
        RtMidiOut midiOut;
        int portNumber = -1;
        if (options.usePortNumber)
        {
            portNumber = options.portNumber;
        }
        else
        {
            portNumber = kmiDevice::findOutputPortNumberByName(midiOut, options.portName);
        }

        const unsigned int numOutPorts = midiOut.getPortCount();
        if (portNumber < 0 || static_cast<unsigned int>(portNumber) >= numOutPorts)
        {
            std::cout << "ERROR: MIDI output port not found. Use -l to list ports.\n";
            return 1;
        }

        const std::string portName = midiOut.getPortName(static_cast<unsigned int>(portNumber));
        std::cout << "Sending BL erase+reboot command (PID=" << options.blPid
                  << ") to port " << portNumber << " (" << portName << ")\n";
        midiOut.openPort(static_cast<unsigned int>(portNumber));
        midiOut.sendMessage(&payload);
        if (options.postDelayMs > 0U)
            std::this_thread::sleep_for(std::chrono::milliseconds(options.postDelayMs));
        midiOut.closePort();
        std::cout << "Done.\n";
    }
    catch (RtMidiError &error)
    {
        std::cout << "ERROR: " << error.getMessage() << "\n";
        return 1;
    }

    return 0;
}

int runManualProcess(const CliOptions &options)
{
    try
    {
        RtMidiOut midiOut;

        if (options.listNormalizedOnly)
        {
            listNormalizedPorts(options.familyName);
            if (options.filePath.empty())
                return 0;
        }

        if (options.listPortsOnly)
        {
            listPorts(midiOut);
            if (options.filePath.empty())
                return 0;
        }

        if (options.filePath.empty())
        {
            std::cout << "ERROR: No SysEx file specified.\n";
            return 1;
        }

        if (!options.usePortNumber && options.portName.empty())
        {
            std::cout << "ERROR: No MIDI output port selected. Use -p <num> or -n <name>.\n";
            return 1;
        }

        int portNumber = -1;
        if (options.usePortNumber)
        {
            portNumber = options.portNumber;
        }
        else if (!options.portName.empty())
        {
            portNumber = kmiDevice::findOutputPortNumberByName(midiOut, options.portName);
        }

        const unsigned int numOutPorts = midiOut.getPortCount();
        if (portNumber < 0 || static_cast<unsigned int>(portNumber) >= numOutPorts)
        {
            std::cout << "ERROR: MIDI output port not found. Use -l to list the raw RtMidi names and numbers.\n";
            return 1;
        }

        std::string errorMessage;
        if (!sendFileToPort(midiOut, portNumber, options.filePath, options.chunkSize, options.chunkDelayMs, options.postDelayMs, errorMessage))
        {
            std::cout << "ERROR: " << errorMessage << "\n";
            return 1;
        }
    }
    catch (RtMidiError &error)
    {
        std::cout << "ERROR: " << error.getMessage() << "\n";
        return 1;
    }

    return 0;
}
}

int main(int argc, const char *argv[])
{
    const CliOptions options = parseArguments(argc, argv);

    if (!options.parseError.empty())
    {
        std::cout << "ERROR: " << options.parseError << "\n";
        if (options.showHelp)
        {
            std::cout << "\n";
            printHelp();
        }
        return 1;
    }

    if (options.showHelp)
    {
        printHelp();
        return 0;
    }

    std::cout << "Chunk size: " << options.chunkSize << " bytes, delay: " << options.chunkDelayMs
              << " ms, post-delay: " << options.postDelayMs << " ms\n";

    if (options.listNormalizedOnly && options.filePath.empty() && options.versionText.empty())
        return runManualProcess(options);

    if (options.idRequestOnly)
        return runIdentityRequest(options);

    if (options.blEraseRebootCmd)
        return runBlEraseRebootCmd(options);

    if (options.automaticMode)
        return runAutomaticProcess(options);

    return runManualProcess(options);
}