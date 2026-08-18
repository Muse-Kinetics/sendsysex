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
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "RtMidi.h"
#include "deviceHelpers.h"
#include "kmiDevice.h"
#include "midiBackend.h"
#include "sysExChunking.h"
#include "chunkedSysExTransfer.h"
#include "version.h"

namespace
{
const std::size_t MAX_MIDI_SYSEX_SIZE = 250000;
// 512 B / 100 ms: validated against real hardware on BopPad (a 49011 B
// monolithic firmware image, sub-split and throttled via chunkedSysExTransfer.h)
// and K-Board (a genuinely multi-chunk Hex_to_SysEx image) - see boppad.json's
// firmware_3_0_3 payload notes and mimic_hub's kboard-perf-test-design.md.
// Applies to both -f/-p/-n (raw send) and --fw-update (automatic update), which
// share this same CliOptions default.
const unsigned int DEFAULT_CHUNK_SIZE = 512;
const unsigned int DEFAULT_CHUNK_DELAY_MS = 100;
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
    unsigned int firstGapDelayMs = 0U;
    unsigned int firstChunkSize = 0U;
    bool postDelayMsExplicit = false;
    bool firstGapDelayMsExplicit = false;
    bool firstChunkSizeExplicit = false;
    bool chunkDelayMsExplicit = false;
    bool blEraseRebootCmd = false;
    int blPid = -1;
    std::string appPortName;
    std::string bootloaderPortName;
    std::string midiBackendOverride;
};


void printHelp()
{
    std::cout
        << "\nSend SysEx Utility v" SENDSYSEX_VERSION
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
        << "  SendSysEx --send-bl-erase-reboot-cmd --pid <pid> -p <num>|-n <name>\n"
        << "  Any command above also accepts --midi-backend <winmm|wms> (Windows only).\n"
        << "\nRaw send mode:\n"
        << "  -l                         List all raw RtMidi input and output ports\n"
        << "  --list-normalized [family] List raw RtMidi port names and their normalized forms\n"
        << "                             Defaults to SoftStep if no family is provided\n"
        << "  -p <num>                   Select a raw RtMidi output port by number\n"
        << "  -n <name>                  Select a raw RtMidi output port by exact name\n"
        << "  -f <file>                  SysEx file to send directly to the selected raw port\n"
        << "\nAutomatic update mode:\n"
        << "  --fw-update <family>       Family name (case-insensitive, non-alphanumeric characters\n"
        << "                             ignored). One of: 12Step, BopPad, K-Board, KBP4,\n"
        << "                             MalletStation, QuNeo, QuNexus, SoftStep\n"
        << "                             (--fw-update=<family> is also accepted)\n"
        << "                             Families may define transport.firmwareUpdateDefaults in their\n"
        << "                             JSON (-fcs/-fgd/-cd/-pd applied automatically, regardless of\n"
        << "                             --midi-backend); currently only KBP4 does, needing a small first\n"
        << "                             window + long settle gap before its receiver's blocking flash-\n"
        << "                             erase op finishes (-fcs 32 -fgd 3000 -cd 150 -pd 3000) - pass\n"
        << "                             any of those flags explicitly on the command line to override.\n"
        << "                             KBP4 also sends Peripheral firmware (while still in application\n"
        << "                             mode, relayed over I2C) then Central firmware (after rebooting\n"
        << "                             into its bootloader).\n"
        << "  --fw-version <version>     Firmware version; omit to use the default from the family JSON\n"
        << "                             (--fw-version=<version> is also accepted)\n"
        << "  --id-request <family>      Connect, send an identity request, print the reply, then exit\n"
        << "  -t <seconds>               Poll interval while waiting for the device to appear or reboot\n"
        << "  --app-port <name>          Exact MIDI port name to use, bypassing family-marker port\n"
        << "                              discovery entirely. For devices whose reported port name has\n"
        << "                              no relationship to their own product string (e.g. a K-Board\n"
        << "                              behind a Mimic Hub, always \"Mimic Hub MIDI Port N\" regardless\n"
        << "                              of what's attached). App/bootloader state is still detected\n"
        << "                              from the identity reply's PID MSB, not from this name.\n"
        << "  --bootloader-port <name>   Exact MIDI port name to use once in bootloader mode, if\n"
        << "                              different from --app-port. Defaults to --app-port's value\n"
        << "                              when omitted (the common case: same port name in both states).\n"
        << "                              Requires --app-port to also be set.\n"
        << "\nTransfer options:\n"
        << "  -cs, --chunk-size <bytes>   SysEx chunk size in bytes (default: 512)\n"
        << "  -cd, --chunk-delay <ms>     Delay between chunks in milliseconds (default: 100)\n"
        << "  -pd, --post-delay <ms>      Wait N ms after last chunk before closing port (default: 500)\n"
        << "                              Prevents F7 loss when the receiver NAKs the final packet.\n"
        << "                              Use 0 to disable.\n"
        << "  -fcs, --first-chunk-size <bytes> Override the size of the very first sub-split window\n"
        << "                              only (default: 0, disabled - uses -cs like every other\n"
        << "                              window). Pairs with -fgd: a small first window that clears\n"
        << "                              the host fast, then a larger -cs for the rest of the\n"
        << "                              transfer once the receiver's settled.\n"
        << "  -fgd, --first-gap-delay <ms> Override delay before the second sub-split window only\n"
        << "                              (default: 0, disabled - uses -cd like every other gap).\n"
        << "                              For devices whose receiver starts a blocking operation\n"
        << "                              (e.g. a flash erase) as soon as it recognizes the message\n"
        << "                              header in the first window, needing several seconds' grace\n"
        << "                              before it can accept the next one. --fw-update applies a\n"
        << "                              family's transport.firmwareUpdateDefaults automatically if its\n"
        << "                              JSON defines one (see above; only KBP4 does today); for raw\n"
        << "                              send (-p/-n -f), always pass them explicitly, e.g. against\n"
        << "                              KBP4: -fcs 32 -cs 512 -cd 150 -fgd 3000 -pd 3000.\n"
        << "\nGlobal options:\n"
        << "  --midi-backend <winmm|wms>  Windows only. Force a specific RtMidi backend instead of\n"
        << "                              auto-detecting. 'wms' fails immediately if the Windows MIDI\n"
        << "                              Services SDK runtime isn't installed/running, rather than\n"
        << "                              silently falling back to WinMM the way auto-detect does.\n"
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
        << "  SendSysEx --fw-update kboard --fw-version 9.0.1 --app-port \"Mimic Hub MIDI Port 5\"\n"
        << "  SendSysEx --id-request QuNexus\n"
        << "  SendSysEx --fw-update BopPad --midi-backend winmm\n"
        << "  SendSysEx --fw-update KBP4\n"
        << "  SendSysEx --send-bl-erase-reboot-cmd --pid <pid>\n\n"
        << "Bootloader commands:\n"
        << "  --send-bl-erase-reboot-cmd  Send a double-EOF erase+reboot command to a KMI bootloader.\n"
        << "                              Requires --pid <pid> (decimal or 0x-prefixed hex).\n"
        << "                              Requires -p or -n to select the MIDI output port.\n\n";
}

void listPorts(RtMidiOut &midiOut)
{
    RtMidiIn midiIn(midiBackend::selectedApi());
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

// SysExChunk and findSysExChunks() now live in sysExChunking.h, shared with
// kmiDevice.cpp's automatic-update path (both need identical F0...F7 message-
// boundary detection over a Hex_to_SysEx-chunked file - see that header for
// the detection-strategy rationale).

// stripTrailingIndex, findMatchingInputPort, ChunkReplyState,
// chunkReplyCallback, waitForIdReply, and sendChunkedFileToPort now live in
// chunkedSysExTransfer.h, shared with kmiDevice.cpp's automatic-update path
// so both send chunked firmware through the identical, hardware-validated
// protocol rather than two independently-drifting implementations.

bool sendFileToPort(RtMidiOut &midiOut,
                    int portNumber,
                    const std::string &filePath,
                    unsigned int chunkSize,
                    unsigned int chunkDelayMs,
                    unsigned int postDelayMs,
                    std::string &errorMessage,
                    unsigned int firstGapDelayMs,
                    unsigned int firstChunkSize)
{
    try
    {
        const std::string portName = midiOut.getPortName(static_cast<unsigned int>(portNumber));

        std::vector<unsigned char> bytes;
        if (!readBinaryFile(filePath, bytes, &errorMessage))
            return false;

        const std::vector<SysExChunk> chunks = findSysExChunks(bytes);

        // Retry (close/reopen/resend-from-scratch on a failed send) now
        // lives in chunkedSysExTransfer.h, shared with kmiDevice's
        // --fw-update path - added 2026-08-12 after a real
        // MidiOutWinMM::sendMessage failure mid-transfer with --midi-backend
        // winmm forced on a machine actually running the WMS translation
        // layer underneath it. Each attempt opens its own fresh RtMidiOut
        // and re-resolves the port by exact name (not the original
        // portNumber), since a retry may follow a device reboot that shifts
        // port indices - the passed-in midiOut is only used above to resolve
        // that name once, never opened directly.
        std::unique_ptr<RtMidiOut> retryOut;
        const bool sent = sendChunkedFileWithRetry(
            portName, bytes, chunks, chunkSize, chunkDelayMs, postDelayMs,
            [&]() -> RtMidiOut * {
                try
                {
                    retryOut.reset(new RtMidiOut(midiBackend::selectedApi()));
                    const unsigned int n = retryOut->getPortCount();
                    for (unsigned int i = 0; i < n; ++i)
                    {
                        if (retryOut->getPortName(i) == portName)
                        {
                            std::cout << "Opening port: " << i << "\n";
                            retryOut->openPort(i);
                            return retryOut.get();
                        }
                    }
                    errorMessage = "MIDI output port not found: " + portName;
                }
                catch (RtMidiError &error)
                {
                    errorMessage = error.getMessage();
                }
                catch (std::exception &error)
                {
                    errorMessage = error.what();
                }
                retryOut.reset();
                return 0;
            },
            [&]() {
                if (retryOut)
                {
                    try { retryOut->closePort(); } catch (...) {}
                }
            },
            errorMessage, firstGapDelayMs, firstChunkSize);

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
            options.chunkDelayMsExplicit = true;
            continue;
        }
        if (arg == "-fcs" || arg == "--first-chunk-size")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for --first-chunk-size.";
                return options;
            }

            if (!parseUnsignedOption(argv[++i], options.firstChunkSize) || options.firstChunkSize > MAX_MIDI_SYSEX_SIZE)
            {
                options.parseError = "Invalid first chunk size. Use a value from 0 (disabled) to " + std::to_string(MAX_MIDI_SYSEX_SIZE) + " bytes.";
                return options;
            }
            options.firstChunkSizeExplicit = true;
            continue;
        }
        if (arg == "-fgd" || arg == "--first-gap-delay")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for --first-gap-delay.";
                return options;
            }

            if (!parseUnsignedOption(argv[++i], options.firstGapDelayMs))
            {
                options.parseError = "Invalid first-gap delay.";
                return options;
            }
            options.firstGapDelayMsExplicit = true;
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
            options.postDelayMsExplicit = true;
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
        if (arg == "--app-port")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for --app-port.";
                return options;
            }
            options.appPortName = argv[++i];
            continue;
        }
        if (arg == "--bootloader-port")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for --bootloader-port.";
                return options;
            }
            options.bootloaderPortName = argv[++i];
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
        if (arg == "--midi-backend")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for --midi-backend.";
                return options;
            }
            options.midiBackendOverride = argv[++i];
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

    if (!options.bootloaderPortName.empty() && options.appPortName.empty())
    {
        std::cout << "ERROR: --bootloader-port requires --app-port to also be set.\n";
        return 1;
    }

    kmiDevice device(options.familyName);

    if (!options.appPortName.empty())
        device.setPortNameOverride(options.appPortName, options.bootloaderPortName);

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

    if (!options.bootloaderPortName.empty() && options.appPortName.empty())
    {
        std::cout << "ERROR: --bootloader-port requires --app-port to also be set.\n";
        return 1;
    }

    kmiDevice device(options.familyName);

    if (!options.appPortName.empty())
    {
        device.setPortNameOverride(options.appPortName, options.bootloaderPortName);
        std::cout << "Port name override active: app=\"" << options.appPortName
                  << "\" bootloader=\""
                  << (options.bootloaderPortName.empty() ? options.appPortName : options.bootloaderPortName)
                  << "\"\n";
    }

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

    // Per-family default chunking/delay profile for --fw-update (e.g. KBP4's
    // Central bootloader/app-relay firmware path, which only accepts a
    // reliable transfer with a small fast first window before its blocking
    // flash-erase op, then a long settle gap - see kbp4.json's
    // transport.firmwareUpdateDefaults for the values and the hardware
    // investigation they came from, 2026-08-15). Generalized 2026-08-18 from
    // a kbp4-only hardcode to a per-family JSON field so any family can opt
    // in without another special case here; families with no such block in
    // their JSON get all-zero defaults below, i.e. behave exactly as before.
    // Applies regardless of --midi-backend, and CLI flags always win if the
    // user passed them explicitly.
    const deviceDatabase::FirmwareUpdateDefaults &familyFwDefaults = device.getFirmwareUpdateDefaults();
    unsigned int firstChunkSize = options.firstChunkSize;
    unsigned int firstGapDelayMs = options.firstGapDelayMs;
    unsigned int postDelayMs = options.postDelayMs;
    unsigned int chunkDelayMs = options.chunkDelayMs;
    if (!options.firstChunkSizeExplicit && familyFwDefaults.firstChunkSize > 0U)
        firstChunkSize = familyFwDefaults.firstChunkSize;
    if (!options.firstGapDelayMsExplicit && familyFwDefaults.firstGapDelayMs > 0U)
        firstGapDelayMs = familyFwDefaults.firstGapDelayMs;
    if (!options.postDelayMsExplicit && familyFwDefaults.postDelayMs > 0U)
        postDelayMs = familyFwDefaults.postDelayMs;
    if (!options.chunkDelayMsExplicit && familyFwDefaults.chunkDelayMs > 0U)
        chunkDelayMs = familyFwDefaults.chunkDelayMs;

    if (!device.runAutomaticUpdate(options.chunkSize, chunkDelayMs, options.pollSeconds, postDelayMs, firstGapDelayMs, firstChunkSize))
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
        RtMidiOut midiOut(midiBackend::selectedApi());
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
        RtMidiOut midiOut(midiBackend::selectedApi());

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
        if (!sendFileToPort(midiOut, portNumber, options.filePath, options.chunkSize, options.chunkDelayMs, options.postDelayMs, errorMessage, options.firstGapDelayMs, options.firstChunkSize))
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
    // stdout is fully buffered (not line-buffered) whenever it's not a live
    // console - which includes every redirect-to-file invocation, so a
    // killed/timed-out run leaves zero output behind even though it ran for
    // minutes. unitbuf flushes after every operator<< instead, at some
    // throughput cost that's irrelevant here (this is a low-rate CLI, not a
    // tight logging loop). Added 2026-08-16 after a stuck --fw-update KBP4
    // background run left an empty output file with no way to tell how far
    // it got before it had to be force-killed.
    std::cout.setf(std::ios_base::unitbuf);

    std::cout << "SendSysEx v" SENDSYSEX_VERSION "\n";

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

    std::string midiBackendError;
    if (!midiBackend::initialize(options.midiBackendOverride, midiBackendError))
    {
        std::cout << "ERROR: " << midiBackendError << "\n";
        return 1;
    }

    std::cout << "MIDI backend: " << midiBackend::describeSelectedApi() << "\n";

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