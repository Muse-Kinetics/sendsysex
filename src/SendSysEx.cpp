//
//  SendSysEx.cpp
//
//  Written by Eric Bateman on 2023/3/31
//  SPDX-License-Identifier: MIT

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <direct.h> // _mkdir
#endif

#include "RtMidi.h"
#include "deviceHelpers.h"
#include "kmiDevice.h"
#include "midiBackend.h"
#include "sysExChunking.h"
#include "chunkedSysExTransfer.h"
#include "bootloaderUpgrade.h"
#include "bootloaderSend.h"
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

    // Legacy "bootloader trojan horse" sector-wise install (see
    // bootloaderUpgrade.h / bootloaderSend.h). --bl-decode is offline (manifest
    // only); --bl-send streams the image to -p/-n one sector at a time.
    bool blDecodeOnly = false;
    bool blSend = false;
    bool blStep = false;
    bool blDryRun = false;
    double blPaddingScale = 1.0; // fraction of the file's inter-packet padding to keep (1=byte-exact, 0=strip)
    unsigned int blBlockToDataGapMs = 40U;
    unsigned int blSectorGapMs = 40U;
    unsigned int blFirstBlockToDataGapMs = 0U;
    unsigned int blBankTransitionMs = 3000U;
    bool blBankProbe = false;
    unsigned int blPreProbeDelayMs = 250U;
    unsigned int blBankProbeTimeoutMs = 12000U;
    bool blProbeFirst = false;       // --probe: version request before sending, abort if silent
    bool blProbeOnly = false;        // --probe-only: probe and report, send no firmware
    unsigned int blProbeTimeoutMs = 4000U; // v93 reply takes ~1.3s (8051 CRC compute); allow margin
    bool blVerify = false;           // --verify: after send, id-request the rebooted device, confirm bootloader
    unsigned int blVerifySettleMs = 6000U; // wait before the first id request: ~5s to install bl + reboot
    unsigned int blVerifyTimeoutS = 15U; // how long to keep retrying the id request after the settle
    bool bootloaderInstall = false;  // --bootloader-install <family>: guided end-to-end trojan install
    std::string blOutDir;            // --out-dir: where the session log + captured image go
    bool timestampOutput = false;    // --timestamp: prefix every line of output with a wall-clock timestamp
};

// Prefixes every line written to std::cout with a "[HH:MM:SS.mmm] " wall-clock
// timestamp, installed by --timestamp. Triggers on '\n' AND '\r' (not just
// '\n') so the progress bar's in-place \r-redraws each get a fresh timestamp
// too - exactly the granularity needed to see how long a real stall lasted
// (e.g. mid-transfer device unresponsiveness) without guessing from wall-
// clock-free log lines. Wraps whatever streambuf std::cout already has, so it
// composes with SessionLog's CoutTee (--bootloader-install's session log)
// regardless of which one gets installed first - timestamps land in both the
// console and the log file when both are active.
class TimestampStreambuf : public std::streambuf
{
public:
    explicit TimestampStreambuf(std::streambuf *dest) : dest_(dest), atLineStart_(true) {}

protected:
    int overflow(int c) override
    {
        if (c == EOF)
            return dest_->pubsync() == 0 ? c : EOF;
        if (atLineStart_ && c != '\n' && c != '\r')
        {
            const std::string ts = currentTimestamp();
            for (std::size_t i = 0; i < ts.size(); ++i)
                if (dest_->sputc(ts[i]) == EOF)
                    return EOF;
            atLineStart_ = false;
        }
        if (c == '\n' || c == '\r')
            atLineStart_ = true;
        return dest_->sputc(static_cast<char>(c)) == EOF ? EOF : c;
    }

    int sync() override { return dest_->pubsync(); }

private:
    static std::string currentTimestamp()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t nowTimeT = std::chrono::system_clock::to_time_t(now);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()) % 1000;
        std::tm localTm{};
#if defined(_WIN32)
        localtime_s(&localTm, &nowTimeT);
#else
        localtime_r(&nowTimeT, &localTm);
#endif
        char buf[16] = {0};
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &localTm);
        char full[24] = {0};
        std::snprintf(full, sizeof(full), "[%s.%03d] ", buf, static_cast<int>(ms.count()));
        return std::string(full);
    }

    std::streambuf *dest_;
    bool atLineStart_;
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
        << "  SendSysEx --bl-decode -f <legacy.syx>\n"
        << "  SendSysEx --bl-send -f <legacy.syx> -p <num>|-n <name> [--step] [--bank-probe] [gaps...]\n"
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
        << "  --bootloader-install <family> Guided end-to-end legacy bootloader (trojan) install: detects\n"
        << "                             the pre-bootloader unit, shows a risk warning + consent prompt,\n"
        << "                             validates the connection (5 version requests) and a firmware\n"
        << "                             dump, installs the bootloader with --verify, then offers to load\n"
        << "                             the latest firmware. Logs the whole session + captured image to\n"
        << "                             --out-dir. SoftStep only for now. On Windows, requires Windows MIDI\n"
        << "                             Services (auto-detect will use it, or pass --midi-backend wms).\n"
        << "  --out-dir <path>           Directory for --bootloader-install's session log + captured\n"
        << "                             image (default: ./bootloader-install-<family>-<timestamp>).\n"
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
        << "\nLegacy \"bootloader trojan horse\" install (pre-1.0.0 SoftStep / 12 Step):\n"
        << "  These units shipped with NO bootloader; a captured monolithic firmware image installs\n"
        << "  one in-place. Unlike modern chunked .syx, the image is ONE F0...F7 of many KMI packets\n"
        << "  (2 banks of FW_HEADER + [FW_BLOCK_HEADER,FW_DATA] 512-byte sectors) and MUST be sent as\n"
        << "  a single message - the receiver resets its stage flag on every F0. WARNING: the update\n"
        << "  erases the reset vector mid-flight; a failure can brick the unit (recoverable only over\n"
        << "  SiLabs USB/JTAG). Send only to a unit you can re-flash.\n"
        << "  On Windows, --bl-send REQUIRES Windows MIDI Services (--midi-backend wms, or auto-detect\n"
        << "  picking it up): WinMM has been observed causing this class of device to surprise-\n"
        << "  disconnect from the USB bus during the blocking flash-patch step, which cannot be\n"
        << "  recovered from mid-transfer. WMS is validated end-to-end on real hardware.\n"
        << "  --bl-decode                 Decode a legacy image and print its bank/sector manifest\n"
        << "                              (offline; opens no port). Use to inspect/verify CRCs.\n"
        << "  --bl-send                   Stream a legacy image (-f) to -p/-n one sector at a time\n"
        << "                              inside a single F0...F7, with the timing below.\n"
        << "  --family <name>             With --bl-send, auto-select the family's control-surface\n"
        << "                              output port instead of -p/-n. Handles legacy SoftStep's\n"
        << "                              unnamed ports (product \"SSCOM\", enumerating as port 1 =\n"
        << "                              control surface / port 2 = expander). E.g. --family SoftStep.\n"
        << "  --step                      Interactive: pause at each sector (Enter=next, N=send N,\n"
        << "                              a=auto-run rest, q=quit) to watch the device sector by sector.\n"
        << "  --probe                     Before sending any firmware, send a firmware-version request\n"
        << "                              (the KMI REQUEST_FW_VERSION the editor uses; the legacy app\n"
        << "                              has no Universal Device Inquiry) and require a reply - aborts\n"
        << "                              the send if the device is silent. Listens on ALL input ports\n"
        << "                              and matches the device's manufacturer signature. Note the v93\n"
        << "                              reply arrives ~1.35s after the request (it computes the app CRC\n"
        << "                              first), so keep --probe-timeout well above that.\n"
        << "  --probe-only                Probe as with --probe, print the result, and STOP - send no\n"
        << "                              firmware. A safe connectivity/version poll (harmless query).\n"
        << "  --verify                    After the send, wait for the device to reboot and re-enumerate,\n"
        << "                              then run the standard id request and confirm it came up in\n"
        << "                              bootloader mode (the successful-install signal). Family is\n"
        << "                              auto-detected from the image (SoftStep/12 Step) or --family.\n"
        << "                              Exit code reflects the verification result.\n"
        << "  --verify-settle <ms>        Wait before the first verify id request (default: 6000). The\n"
        << "                              trojan takes ~5s to install the bootloader and reboot; querying\n"
        << "                              earlier just misses the still-rebooting device.\n"
        << "  --verify-timeout <s>        Seconds to keep retrying the id request after the settle\n"
        << "                              (default: 15).\n"
        << "  --probe-timeout <ms>        How long to wait for the pre-send probe reply (default: 4000).\n"
        << "                              The v93 reply takes ~1.3s (it computes the app CRC first), so\n"
        << "                              do not set this below ~2000.\n"
        << "  --block-data-gap <ms>       Gap after each FW_BLOCK_HEADER, before its FW_DATA - the\n"
        << "                              sector's flash-erase time (default: 40).\n"
        << "  --first-block-data-gap <ms> Override --block-data-gap for the very first sector only\n"
        << "                              (default: 0 = use --block-data-gap).\n"
        << "  --sector-gap <ms>           Gap after each FW_DATA, write/settle time (default: 40).\n"
        << "  --bank-gap <ms>             Blind wait at the bank 0->1 transition (default: 3000).\n"
        << "                              Superseded by --bank-probe when that is set.\n"
        << "  --bank-probe                At the bank transition, wait --pre-probe-delay, then send a\n"
        << "                              REQUEST_FW_VERSION packet (inside the open message) and wait\n"
        << "                              for the device's reply before bank 1 - confirms the staged\n"
        << "                              firmware rebooted (the only reliable stage 1->2 sync).\n"
        << "                              Requires the matching MIDI input port.\n"
        << "  --pre-probe-delay <ms>      Wait after bank 0's last sector BEFORE the first probe, to\n"
        << "                              let the staged firmware finish its reboot/re-init deaf\n"
        << "                              window (default: 250).\n"
        << "  --bank-probe-timeout <ms>   Total budget to get a probe reply before aborting (default:\n"
        << "                              12000). The request is resent periodically while waiting.\n"
        << "  --padding-scale <0..1>      Fraction of the image's baked-in inter-packet padding to keep\n"
        << "                              (default 1.0 = keep all, so the wire bytes are identical to the\n"
        << "                              source file - the proven-safe stream - with gaps added on top).\n"
        << "                              0.5 keeps half of each packet's padding; 0.0 keeps none. Use to\n"
        << "                              experiment with how much padding the gaps can replace.\n"
        << "  --strip-padding             Alias for --padding-scale 0. Drops ~19.7 KB of padding, which\n"
        << "                              on real hardware misframes and reboots the unit - experiment only.\n"
        << "  --dry-run                   Print the send schedule without opening a port or sending.\n"
        << "  (-pd/--post-delay also applies here: wait after the final F7.)\n"
        << "\nGlobal options:\n"
        << "  --midi-backend <winmm|wms>  Windows only. Force a specific RtMidi backend instead of\n"
        << "                              auto-detecting. 'wms' fails immediately if the Windows MIDI\n"
        << "                              Services SDK runtime isn't installed/running, rather than\n"
        << "                              silently falling back to WinMM the way auto-detect does.\n"
        << "  --timestamp                 Prefix every line of output with a [HH:MM:SS.mmm] wall-clock\n"
        << "                              timestamp, including in-place progress-bar redraws. For\n"
        << "                              diagnosing exactly how long a real stall lasted on hardware.\n"
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
        if (arg == "--bootloader-install")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing family for --bootloader-install.";
                return options;
            }
            options.bootloaderInstall = true;
            options.familyName = argv[++i];
            continue;
        }
        if (arg == "--out-dir")
        {
            if (i + 1 >= argc)
            {
                options.parseError = "Missing value for --out-dir.";
                return options;
            }
            options.blOutDir = argv[++i];
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
        if (arg == "--timestamp")
        {
            options.timestampOutput = true;
            continue;
        }
        if (arg == "--bl-decode")
        {
            options.blDecodeOnly = true;
            continue;
        }
        if (arg == "--bl-send")
        {
            options.blSend = true;
            continue;
        }
        if (arg == "--step")
        {
            options.blStep = true;
            continue;
        }
        if (arg == "--dry-run")
        {
            options.blDryRun = true;
            continue;
        }
        if (arg == "--strip-padding")
        {
            options.blPaddingScale = 0.0; // alias for --padding-scale 0
            continue;
        }
        if (arg == "--padding-scale")
        {
            if (i + 1 >= argc)
            {
                options.parseError = "Missing value for --padding-scale.";
                return options;
            }
            char *end = 0;
            const double v = std::strtod(argv[++i], &end);
            if (!end || *end != '\0' || v < 0.0 || v > 1.0)
            {
                options.parseError = "Invalid --padding-scale (use a fraction from 0.0 to 1.0).";
                return options;
            }
            options.blPaddingScale = v;
            continue;
        }
        if (arg == "--block-data-gap")
        {
            if (i + 1 >= argc || !parseUnsignedOption(argv[++i], options.blBlockToDataGapMs))
            {
                options.parseError = "Invalid or missing value for --block-data-gap.";
                return options;
            }
            continue;
        }
        if (arg == "--first-block-data-gap")
        {
            if (i + 1 >= argc || !parseUnsignedOption(argv[++i], options.blFirstBlockToDataGapMs))
            {
                options.parseError = "Invalid or missing value for --first-block-data-gap.";
                return options;
            }
            continue;
        }
        if (arg == "--sector-gap")
        {
            if (i + 1 >= argc || !parseUnsignedOption(argv[++i], options.blSectorGapMs))
            {
                options.parseError = "Invalid or missing value for --sector-gap.";
                return options;
            }
            continue;
        }
        if (arg == "--bank-gap")
        {
            if (i + 1 >= argc || !parseUnsignedOption(argv[++i], options.blBankTransitionMs))
            {
                options.parseError = "Invalid or missing value for --bank-gap.";
                return options;
            }
            continue;
        }
        if (arg == "--bank-probe")
        {
            options.blBankProbe = true;
            continue;
        }
        if (arg == "--probe")
        {
            options.blProbeFirst = true;
            continue;
        }
        if (arg == "--probe-only")
        {
            options.blProbeFirst = true;
            options.blProbeOnly = true;
            continue;
        }
        if (arg == "--verify")
        {
            options.blVerify = true;
            continue;
        }
        if (arg == "--verify-timeout")
        {
            if (i + 1 >= argc || !parseUnsignedOption(argv[++i], options.blVerifyTimeoutS))
            {
                options.parseError = "Invalid or missing value for --verify-timeout.";
                return options;
            }
            continue;
        }
        if (arg == "--verify-settle")
        {
            if (i + 1 >= argc || !parseUnsignedOption(argv[++i], options.blVerifySettleMs))
            {
                options.parseError = "Invalid or missing value for --verify-settle.";
                return options;
            }
            continue;
        }
        if (arg == "--probe-timeout")
        {
            if (i + 1 >= argc || !parseUnsignedOption(argv[++i], options.blProbeTimeoutMs))
            {
                options.parseError = "Invalid or missing value for --probe-timeout.";
                return options;
            }
            continue;
        }
        if (arg == "--pre-probe-delay")
        {
            if (i + 1 >= argc || !parseUnsignedOption(argv[++i], options.blPreProbeDelayMs))
            {
                options.parseError = "Invalid or missing value for --pre-probe-delay.";
                return options;
            }
            continue;
        }
        if (arg == "--bank-probe-timeout")
        {
            if (i + 1 >= argc || !parseUnsignedOption(argv[++i], options.blBankProbeTimeoutMs))
            {
                options.parseError = "Invalid or missing value for --bank-probe-timeout.";
                return options;
            }
            continue;
        }
        if (arg == "--family")
        {
            if (i + 1 >= argc)
            {
                options.showHelp = true;
                options.parseError = "Missing value for --family.";
                return options;
            }
            options.familyName = argv[++i];
            continue;
        }
        options.showHelp = true;
        options.parseError = "Unrecognized argument: " + arg;
        return options;
    }

    return options;
}

// Legacy (pre-bootloader) firmware-version query for SoftStep / 12 Step. These
// units do NOT answer the standard KMI/Universal id request - only the device-
// specific REQUEST_FW_VERSION the editor uses (KMI_SysexMessages.c _fw_req_*).
// Request bytes are verbatim: F0 + KMI header + 50 zero pad + REQUEST_FW_VERSION
// packet + F7. The reply is a FW_HEADER; its ASCII version digits are < 0x80 so
// they survive 7-bit encoding and can be read straight from the raw reply.
struct LegacyReplyState
{
    volatile bool matched;
    std::vector<unsigned char> sig;   // manufacturer signature to match (e.g. 1B 48 7A 01)
    std::vector<unsigned char> reply; // captured bytes of the matching message
    LegacyReplyState() : matched(false) {}
};

static void legacyReplyCallback(double, std::vector<unsigned char> *msg, void *user)
{
    LegacyReplyState *s = static_cast<LegacyReplyState *>(user);
    if (s->matched || msg == 0 || msg->size() < s->sig.size())
        return;
    for (std::size_t i = 0; i + s->sig.size() <= msg->size(); ++i)
    {
        bool ok = true;
        for (std::size_t j = 0; j < s->sig.size(); ++j)
            if ((*msg)[i + j] != s->sig[j]) { ok = false; break; }
        if (ok) { s->reply = *msg; s->matched = true; return; }
    }
}

// Returns true and fills versionOut ("93", etc.) if a legacy unit answers.
static bool runLegacyVersionQuery(const std::string &familyId, std::string &versionOut)
{
    std::vector<unsigned char> req, sig, outMarker;
    if (familyId == "softstep")
    {
        req = {0xF0,0x00,0x1B,0x48,0x7A,0x01,0x00};
        sig = {0x1B,0x48,0x7A,0x01};
    }
    else if (familyId == "12step")
    {
        req = {0xF0,0x00,0x01,0x55,0x7A,0x14,0x00};
        sig = {0x00,0x01,0x55,0x7A,0x14};
    }
    else
        return false;
    req.resize(req.size() + 50, 0x00); // 50-zero pad
    const unsigned char pkt[] = {0x01,0x00,0x00,0x00,0x00,0x04,0x40,0x00,0x30};
    req.insert(req.end(), pkt, pkt + sizeof(pkt));
    req.push_back(0xF7);

    // Find an output port to send to (any port carrying the family marker) and
    // open every input to catch the reply wherever it lands.
    RtMidiOut out(midiBackend::selectedApi());
    int outIdx = -1;
    const std::string marker = (familyId == "softstep") ? "SSCOM" : "12";
    for (unsigned int i = 0; i < out.getPortCount(); ++i)
        if (out.getPortName(i).find(marker) != std::string::npos) { outIdx = static_cast<int>(i); break; }
    if (outIdx < 0)
        return false;
    out.openPort(static_cast<unsigned int>(outIdx));

    LegacyReplyState state;
    state.sig = sig;
    std::vector<RtMidiIn *> ins;
    RtMidiIn probe(midiBackend::selectedApi());
    for (unsigned int i = 0; i < probe.getPortCount(); ++i)
    {
        RtMidiIn *in = 0;
        try { in = new RtMidiIn(midiBackend::selectedApi()); in->ignoreTypes(false,false,false);
              in->openPort(i); in->setCallback(legacyReplyCallback, &state); ins.push_back(in); }
        catch (...) { delete in; }
    }

    std::vector<unsigned char> msg(req.begin(), req.end());
    out.sendMessage(&msg);
    for (int waited = 0; waited < 3000 && !state.matched; waited += 5)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    for (std::size_t i = 0; i < ins.size(); ++i) { try { ins[i]->closePort(); } catch (...) {} delete ins[i]; }

    if (!state.matched)
        return false;
    // Scan the reply for two consecutive ASCII digits = the version string.
    for (std::size_t i = 0; i + 1 < state.reply.size(); ++i)
        if (state.reply[i] >= 0x30 && state.reply[i] <= 0x39 &&
            state.reply[i + 1] >= 0x30 && state.reply[i + 1] <= 0x39)
        { versionOut = std::string(1, (char)state.reply[i]) + (char)state.reply[i + 1]; break; }
    return true;
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

    // refreshPorts already sends the identity request and waits up to 500 ms.
    // If the reply has not arrived yet (slow device), poll a bit longer.
    if (device.getState() != kmiDevice::State::disconnected && !device.hasReceivedIdentity())
    {
        std::cout << "Waiting for identity reply...\n";
        for (int i = 0; i < 40 && !device.hasReceivedIdentity(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (device.hasReceivedIdentity())
    {
        device.printIdentityMetadata();
        return 0;
    }

    // No modern reply: the device may be a pre-bootloader legacy unit (SoftStep
    // SSCOM / old 12 Step) that only answers the device-specific REQUEST_FW_VERSION.
    // Try that before giving up - this is the "correct message for the ports we see".
    std::string legacyVer;
    if (runLegacyVersionQuery(normalizeFamilyId(options.familyName), legacyVer))
    {
        std::cout << "Legacy (pre-bootloader) " << options.familyName << " firmware detected"
                  << (legacyVer.empty() ? "" : (" - version " + legacyVer)) << ".\n"
                  << "  Answers REQUEST_FW_VERSION, not the standard id request; no bootloader installed"
                     " (a trojan bl-send target).\n";
        return 0;
    }

    if (device.getState() == kmiDevice::State::disconnected)
        std::cout << "ERROR: " << (device.getLastError().empty() ? "Device not found." : device.getLastError()) << "\n";
    else
        std::cout << "No identity reply received within the timeout.\n";
    return 1;
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

// Decode a legacy "bootloader trojan horse" .syx and print its sector
// manifest. Offline - opens no MIDI port.
int runBlDecode(const CliOptions &options)
{
    if (options.filePath.empty())
    {
        std::cout << "ERROR: --bl-decode requires -f <file.syx>.\n";
        return 1;
    }
    std::vector<unsigned char> bytes;
    std::string readError;
    if (!readBinaryFile(options.filePath, bytes, &readError))
    {
        std::cout << "ERROR: " << readError << "\n";
        return 1;
    }
    const blupgrade::LegacyFirmwareImage img =
        blupgrade::decodeLegacyFirmwareSyx(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    blupgrade::printLegacyImageManifest(img);
    return (img.ok && img.allCrcOk()) ? 0 : 1;
}

// Resolve the family's application "control surface" output port by name
// normalization only - no identity handshake, so it works on a legacy v93
// unit that doesn't answer modern identity requests. Legacy SoftStep units
// enumerate as the bare product string "SSCOM" with two UNNAMED jacks
// (iJack=0 in the v93 USB descriptor), which the OS numbers as port 1
// (control surface) / port 2 (expander); the softstep.json
// legacy_sscom_application profile models exactly that, and chooseBestPort()
// picks the control_surface role (safeProbeRoles.application order:
// control_surface, main, port1). Returns false (and fills err) if the family
// JSON is missing or no matching port is visible.
bool resolveFamilyControlSurfacePort(const std::string &family, RtMidiOut &midiOut,
                                     std::string &portNameOut, std::string &err)
{
    deviceDatabase db;
    if (!db.loadFamily(family))
    {
        err = db.getLastError();
        return false;
    }
    db.setActiveApi(midiOut.getCurrentApi());

    std::vector<std::string> matched;
    const unsigned int n = midiOut.getPortCount();
    for (unsigned int i = 0; i < n; ++i)
    {
        const std::string name = midiOut.getPortName(i);
        if (db.matchesFamily(name))
            matched.push_back(name);
    }
    if (matched.empty())
    {
        err = "No visible MIDI output port matched family \"" + family + "\".";
        return false;
    }
    const std::string best = db.chooseBestPort(matched, false /* application state */);
    if (best.empty())
    {
        err = "Found family ports but none resolved to a control-surface/main role.";
        return false;
    }
    portNameOut = best;
    return true;
}

// Stream a legacy image to -p/-n (or an auto-resolved --family port) one sector
// at a time inside a single F0..F7, with operator-controlled timing (--step or
// timed gaps). See bootloaderSend.h.
// Identify the family from a decoded legacy image's outer header manufacturer
// bytes (F0 00 mfg2 mfg3 mfg4 ...): SoftStep = 1B 48 7A, 12 Step = 01 55 7A. Used
// so --verify can find the rebooted device without an explicit --family.
static std::string familyFromImage(const blupgrade::LegacyFirmwareImage &img)
{
    if (img.outerHeaderWire.size() >= 5)
    {
        const uint8_t a = img.outerHeaderWire[2], b = img.outerHeaderWire[3], c = img.outerHeaderWire[4];
        if (a == 0x1B && b == 0x48 && c == 0x7A) return "SoftStep";
        if (a == 0x01 && b == 0x55 && c == 0x7A) return "12Step";
    }
    return "";
}

// After a --bl-send trojan install the device stages the bootloader and reboots -
// ~5s on real hardware - re-enumerating as its bootloader port. We therefore wait
// settleMs BEFORE the first id request (querying mid-reboot just misses), then
// retry: each attempt uses a fresh kmiDevice so refreshPorts() re-sends the
// inquiry (kmiDevice won't resend on one instance once a request is pending).
// Success = a reply in bootloader state (reply PID MSB flipped; see softstep.json
// stateDetection.bootloaderPidMsb). Returns 0 on a confirmed bootloader, else 1.
static int verifyInstallViaIdRequest(const std::string &familyName,
                                     unsigned int settleMs, unsigned int timeoutS)
{
    std::cout << "\nVerifying install: waiting " << (settleMs / 1000.0)
              << "s for the device to install the bootloader and reboot ...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(settleMs));

    unsigned int waited = 0;
    const unsigned int attemptGapMs = 500;
    bool responded = false;
    kmiDevice::State state = kmiDevice::State::disconnected;
    while (waited < timeoutS * 1000)
    {
        kmiDevice device(familyName); // fresh -> refreshPorts() sends a new inquiry
        if (device.refreshPorts() && device.getState() != kmiDevice::State::disconnected)
        {
            for (int i = 0; i < 16 && !device.hasReceivedIdentity(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (device.hasReceivedIdentity())
            {
                device.printIdentityMetadata();
                responded = true;
                state = device.getState();
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(attemptGapMs));
        waited += attemptGapMs + 800; // account for the in-loop wait above
    }

    if (!responded)
    {
        std::cout << "VERIFY FAILED: no identity reply within the window - the device did not come "
                     "back up as a bootloader (try a longer --verify-settle/--verify-timeout).\n";
        return 1;
    }
    if (state == kmiDevice::State::bootloader)
    {
        std::cout << "VERIFY OK: device is in bootloader mode - trojan install succeeded.\n";
        return 0;
    }
    std::cout << "VERIFY WARNING: device responded but is NOT in bootloader mode; "
                 "the install may not have taken.\n";
    return 1;
}

int runBlSend(const CliOptions &options)
{
    if (options.filePath.empty())
    {
        std::cout << "ERROR: --bl-send requires -f <file.syx>.\n";
        return 1;
    }
    const bool haveExplicitPort = options.usePortNumber || !options.portName.empty();
    if (!options.blDryRun && !haveExplicitPort && options.familyName.empty())
    {
        std::cout << "ERROR: --bl-send needs a MIDI output port: -p <num>, -n <name>, "
                     "or --family <name> to auto-select (e.g. --family SoftStep).\n";
        return 1;
    }

    std::vector<unsigned char> bytes;
    std::string readError;
    if (!readBinaryFile(options.filePath, bytes, &readError))
    {
        std::cout << "ERROR: " << readError << "\n";
        return 1;
    }

    const blupgrade::LegacyFirmwareImage img =
        blupgrade::decodeLegacyFirmwareSyx(std::vector<uint8_t>(bytes.begin(), bytes.end()));
    if (!img.ok)
    {
        std::cout << "ERROR: could not decode legacy image: " << img.error << "\n";
        return 1;
    }
    if (!img.allCrcOk())
    {
        std::cout << "ERROR: decoded image has CRC failures - refusing to send a corrupt image. "
                     "Run --bl-decode " << options.filePath << " to inspect.\n";
        return 1;
    }

#if defined(_WIN32)
    // Real-hardware finding (2026-08-25): under WinMM, this device genuinely
    // surprise-disconnects from the USB bus (confirmed via the Kernel-PnP
    // "surprise removed" event) during the blocking CRC-verify/flash-patch/
    // jump at the bank-0 tail - not a driver busy signal we can retry through.
    // Once the device is torn down mid-message there is no way to resume: a
    // fresh port means a fresh F0, which re-triggers the receiver's erase-on-
    // header behavior and corrupts whatever was already staged. WMS does not
    // exhibit this and has been validated end-to-end (both banks + --verify)
    // on real hardware, so it's required here rather than chasing the WinMM
    // failure further.
    if (!options.blDryRun && midiBackend::selectedApi() != RtMidi::WINDOWS_MIDI_SERVICES)
    {
        const std::string deviceLabel = options.familyName.empty() ? std::string("legacy") : options.familyName;
        std::cout << "ERROR: the " << deviceLabel << " bootloader upgrade requires Windows MIDI Services. "
                     "You may need to download and install the SDK from "
                     "https://microsoft.github.io/MIDI/get-latest/, then try again.\n";
        return 1;
    }
#endif

    BlSendOptions sendOpts;
    sendOpts.step = options.blStep;
    sendOpts.dryRun = options.blDryRun;
    sendOpts.blockToDataGapMs = options.blBlockToDataGapMs;
    sendOpts.sectorGapMs = options.blSectorGapMs;
    sendOpts.firstBlockToDataGapMs = options.blFirstBlockToDataGapMs;
    sendOpts.bankTransitionMs = options.blBankTransitionMs;
    sendOpts.postDelayMs = options.postDelayMs;
    sendOpts.bankProbe = options.blBankProbe;
    sendOpts.preProbeDelayMs = options.blPreProbeDelayMs;
    sendOpts.bankProbeTimeoutMs = options.blBankProbeTimeoutMs;
    sendOpts.probeFirst = options.blProbeFirst;
    sendOpts.probeOnly = options.blProbeOnly;
    sendOpts.probeTimeoutMs = options.blProbeTimeoutMs;
    sendOpts.paddingScale = options.blPaddingScale;

    try
    {
        RtMidiOut midiOut(midiBackend::selectedApi());

        // Dry run inspects the send schedule without a device, so don't insist
        // on resolving/opening a real port.
        std::string portName = options.portName.empty() ? "DRY-RUN" : options.portName;
        if (!sendOpts.dryRun)
        {
            int portNumber = -1;
            if (options.usePortNumber)
            {
                portNumber = options.portNumber;
            }
            else if (!options.portName.empty())
            {
                portNumber = kmiDevice::findOutputPortNumberByName(midiOut, options.portName);
            }
            else if (!options.familyName.empty())
            {
                // Auto-select the family's control-surface output port (handles
                // legacy SSCOM's unnamed "port 1 / port 2" enumeration).
                std::string resolvedName, resolveErr;
                if (!resolveFamilyControlSurfacePort(options.familyName, midiOut, resolvedName, resolveErr))
                {
                    std::cout << "ERROR: " << resolveErr << " Use -l to list ports, or pass -n/-p.\n";
                    return 1;
                }
                std::cout << "Auto-selected " << options.familyName << " port: \"" << resolvedName << "\"\n";
                portNumber = kmiDevice::findOutputPortNumberByName(midiOut, resolvedName);
            }

            const unsigned int numOutPorts = midiOut.getPortCount();
            if (portNumber < 0 || static_cast<unsigned int>(portNumber) >= numOutPorts)
            {
                std::cout << "ERROR: MIDI output port not found. Use -l to list ports.\n";
                return 1;
            }
            portName = midiOut.getPortName(static_cast<unsigned int>(portNumber));
            midiOut.openPort(static_cast<unsigned int>(portNumber));
        }

        std::string sendError;
        if (!sendLegacyImageSectorwise(midiOut, portName, img, sendOpts, sendError))
        {
            std::cout << "ERROR: " << sendError << "\n";
            return 1;
        }
    }
    catch (RtMidiError &error)
    {
        std::cout << "ERROR: " << error.getMessage() << "\n";
        return 1;
    }

    // Post-send verification: the device has closed its send port and rebooted;
    // confirm it came back up in its bootloader. (Skip for --probe-only, which
    // never sent firmware, and for dry runs.)
    if (options.blVerify && !options.blDryRun && !options.blProbeOnly)
    {
        const std::string fam = !options.familyName.empty() ? options.familyName : familyFromImage(img);
        if (fam.empty())
        {
            std::cout << "VERIFY SKIPPED: could not determine family from the image; "
                         "pass --family to enable verification.\n";
            return 0;
        }
        return verifyInstallViaIdRequest(fam, options.blVerifySettleMs, options.blVerifyTimeoutS);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// --bootloader-install <family>: guided, logged, end-to-end legacy trojan
// bootloader install. macOS only for now (this guided flow, including its
// --verify step, is validated only on macOS; WinMM/WMS validation of the
// underlying --bl-send send path is separate and still in progress). SoftStep
// supported today.
// ---------------------------------------------------------------------------

// 75-byte SoftStep firmware-dump request (ss_firmware_update_request.syx): asks a
// legacy unit to send its own firmware image back, used as a second connectivity
// proof before we commit to flashing.
static const unsigned char kSoftStepDumpRequest[] = {
    0xF0, 0x00, 0x1B, 0x48, 0x7A, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x33, 0x70,
    0x00, 0x30, 0xF7,
};

// Duplicates everything written to std::cout into a log file, so the whole
// session (all our output, and the helpers' output) is captured. User input read
// via std::cin is logged separately (logInput), since cin doesn't pass through cout.
class CoutTee : public std::streambuf
{
public:
    CoutTee(std::streambuf *console, std::streambuf *file) : console_(console), file_(file) {}
protected:
    int overflow(int c) override
    {
        if (c == EOF) return !EOF;
        const int r1 = console_->sputc(static_cast<char>(c));
        const int r2 = file_->sputc(static_cast<char>(c));
        return (r1 == EOF || r2 == EOF) ? EOF : c;
    }
    int sync() override { return (console_->pubsync() == 0 && file_->pubsync() == 0) ? 0 : -1; }
private:
    std::streambuf *console_, *file_;
};

struct SessionLog
{
    std::ofstream file;
    std::streambuf *old;
    CoutTee *tee;
    SessionLog(const std::string &path) : old(std::cout.rdbuf()), tee(0)
    {
        file.open(path.c_str(), std::ios::out | std::ios::app);
        if (file.is_open())
        {
            tee = new CoutTee(old, file.rdbuf());
            std::cout.rdbuf(tee);
        }
    }
    ~SessionLog() { std::cout.rdbuf(old); delete tee; }
    void logInput(const std::string &s) { if (file.is_open()) { file << s << "\n"; file.flush(); } }
};

// Send the family's firmware-dump request and capture the returned image (one
// F0..F7) on whichever input port it arrives on. Returns true if a complete
// message was captured into out.
static bool captureFirmwareDump(const std::string &familyId, std::vector<unsigned char> &out)
{
    if (familyId != "softstep")
        return false;

    struct DumpState
    {
        volatile bool done;
        std::vector<unsigned char> buf;
        DumpState() : done(false) {}
    } state;

    struct CB
    {
        static void proc(double, std::vector<unsigned char> *m, void *u)
        {
            DumpState *s = static_cast<DumpState *>(u);
            if (s->done || m == 0 || m->size() < 4 || m->front() != 0xF0)
                return;
            s->buf = *m;
            s->done = true;
        }
    };

    RtMidiOut out_port(midiBackend::selectedApi());
    int outIdx = -1;
    for (unsigned int i = 0; i < out_port.getPortCount(); ++i)
        if (out_port.getPortName(i).find("SSCOM") != std::string::npos) { outIdx = static_cast<int>(i); break; }
    if (outIdx < 0)
        return false;
    out_port.openPort(static_cast<unsigned int>(outIdx));

    std::vector<RtMidiIn *> ins;
    RtMidiIn probe(midiBackend::selectedApi());
    for (unsigned int i = 0; i < probe.getPortCount(); ++i)
    {
        RtMidiIn *in = 0;
        try
        {
            in = new RtMidiIn(midiBackend::selectedApi());
            in->ignoreTypes(false, false, false);
            in->openPort(i);
            in->setCallback(CB::proc, &state);
            ins.push_back(in);
        }
        catch (...) { delete in; }
    }

    std::vector<unsigned char> req(kSoftStepDumpRequest, kSoftStepDumpRequest + sizeof(kSoftStepDumpRequest));
    out_port.sendMessage(&req);
    for (int waited = 0; waited < 15000 && !state.done; waited += 20)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    for (std::size_t i = 0; i < ins.size(); ++i) { try { ins[i]->closePort(); } catch (...) {} delete ins[i]; }

    if (!state.done)
        return false;
    out.swap(state.buf);
    return true;
}

// Build the default output directory: ./bootloader-install-<family>-<timestamp>.
static std::string defaultOutDir(const std::string &familyId)
{
    std::time_t t = std::time(0);
    std::tm *lt = std::localtime(&t);
    char stamp[32] = {0};
    if (lt)
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", lt);
    return "bootloader-install-" + familyId + "-" + stamp;
}

// Best-effort directory creation (ignores "already exists" - callers don't
// need to know which). POSIX mkdir(path, mode) vs. Windows _mkdir(path) (no
// mode parameter) have different signatures, so this can't be a single
// unconditional call the way most of this file's I/O is.
static void makeDirectoryBestEffort(const std::string &path)
{
#if defined(_WIN32)
    ::_mkdir(path.c_str());
#else
    ::mkdir(path.c_str(), 0755);
#endif
}

static std::string trimUpper(const std::string &s)
{
    std::string t = trimCopy(s);
    for (std::size_t i = 0; i < t.size(); ++i)
        t[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[i])));
    return t;
}

int runBootloaderInstall(const CliOptions &options)
{
    const std::string fam = normalizeFamilyId(options.familyName);
    if (fam != "softstep")
    {
        std::cout << "ERROR: --bootloader-install currently supports SoftStep only "
                     "(got \"" << options.familyName << "\").\n";
        return 1;
    }
#if defined(_WIN32)
    // Fail fast, before the interactive consent/validation steps below - see
    // the matching check in runBlSend() for why WMS is required on Windows.
    if (midiBackend::selectedApi() != RtMidi::WINDOWS_MIDI_SERVICES)
    {
        std::cout << "ERROR: the " << options.familyName << " bootloader upgrade requires Windows MIDI Services. "
                     "You may need to download and install the SDK from "
                     "https://microsoft.github.io/MIDI/get-latest/, then try again.\n";
        return 1;
    }
#endif
    // Output directory + full-session log tee (captures all cout + user input).
    const std::string outDir = options.blOutDir.empty() ? defaultOutDir(fam) : options.blOutDir;
    makeDirectoryBestEffort(outDir);
    const std::string logPath = outDir + "/session.log";
    SessionLog log(logPath);

    std::cout << "=== SoftStep Bootloader Install ===\n";
    std::cout << "Session log: " << logPath << "\n\n";

    // 1) Detect the legacy (pre-bootloader) unit and its firmware version.
    std::string ver;
    if (!runLegacyVersionQuery(fam, ver))
    {
        std::cout << "ERROR: no legacy (pre-bootloader) SoftStep detected. Connect an SSCOM-era unit "
                     "and try again.\n";
        return 1;
    }
    if (ver.empty())
        ver = "unknown";

    // 2) Warning + explicit consent gate.
    std::cout << "WARNING: your device is running legacy firmware version " << ver
              << ", which does not have a bootloader. Updating firmware without a bootloader is risky,"
                 " and if interrupted can brick your device. This utility does it's best to install a"
                 " bootloader safely, but proceed with caution. Close ALL programs except for this"
                 " update, disconnect all USB devices, and ensure that your cable connection is secure"
                 " and that you are using a known good cable. If the update fails, you can submit a"
                 " support ticket at https://support.musekinetics.com and arrange to ship the unit back"
                 " to our office in California, but you will need to cover the shipping costs there and"
                 " back.\n\n";
    std::cout << "To proceed, please type \"I UNDERSTAND THE RISKS\": " << std::flush;
    std::string consent;
    std::getline(std::cin, consent);
    log.logInput(consent);
    if (trimUpper(consent) != "I UNDERSTAND THE RISKS")
    {
        std::cout << "Aborted\n";
        return 1;
    }

    // 3) Connection validation: five version requests, 2 s apart.
    std::cout << "\nValidating connection (5 firmware-version requests, 2s apart) ...\n";
    int replies = 0;
    for (int i = 1; i <= 5; ++i)
    {
        std::string v;
        const bool got = runLegacyVersionQuery(fam, v);
        std::cout << "  [" << i << "/5] " << (got ? ("reply - version " + (v.empty() ? std::string("?") : v))
                                                   : std::string("NO REPLY")) << "\n";
        if (got)
            ++replies;
        if (i < 5)
            std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (replies < 5)
    {
        std::cout << "Connection unstable (" << replies << "/5 replied). Aborting for safety - check the "
                     "cable/port and retry.\n";
        return 1;
    }
    std::cout << "Connection stable (5/5 replied).\n";

    // 4) Second validation: ask the device to dump its own firmware image and
    //    sanity-check it (single F0..F7 with the KMI SoftStep header).
    std::cout << "\nRequesting the device's current firmware image (dump validation) ...\n";
    std::vector<unsigned char> dump;
    if (!captureFirmwareDump(fam, dump))
    {
        std::cout << "ERROR: the device did not return a firmware image. Aborting before flashing.\n";
        return 1;
    }
    const std::string dumpPath = outDir + "/device-dump-v" + ver + ".syx";
    {
        std::ofstream df(dumpPath.c_str(), std::ios::binary);
        df.write(reinterpret_cast<const char *>(dump.data()), static_cast<std::streamsize>(dump.size()));
    }
    const bool dumpValid = dump.size() > 10 && dump.front() == 0xF0 && dump.back() == 0xF7 &&
                           dump[1] == 0x00 && dump[2] == 0x1B && dump[3] == 0x48 && dump[4] == 0x7A;
    std::cout << "Captured " << dump.size() << " bytes -> " << dumpPath << "\n";
    if (!dumpValid)
    {
        std::cout << "ERROR: captured image failed validation (expected F0 00 1B 48 7A ... F7). Aborting.\n";
        return 1;
    }
    std::cout << "Image validated (F0 ... F7, KMI SoftStep header present).\n";

    // 5) Install the trojan bootloader via --bl-send with the proven macOS timing
    //    preset and post-send --verify. Resolve the trojan from the family JSON.
    std::string trojanPath;
    {
        deviceDatabase db;
        if (!db.loadFamily(fam) || !db.getPayloadPath("bootloader_installer", 0, trojanPath) || trojanPath.empty())
        {
            std::cout << "ERROR: could not locate the bootloader installer image in the device database.\n";
            return 1;
        }
    }

    std::cout << "\nInstalling bootloader (this takes ~30-40s; do not disconnect) ...\n";
    CliOptions bl = options;
    bl.bootloaderInstall = false;
    bl.blSend = true;
    bl.familyName = "SoftStep";
    bl.filePath = trojanPath;
    bl.portName.clear();
    bl.usePortNumber = false;
    bl.blPaddingScale = 1.0;             // byte-exact (proven)
    bl.blFirstBlockToDataGapMs = 1000;   // proven on both macOS and Windows/WMS
    bl.blBlockToDataGapMs = 100;
    bl.blSectorGapMs = 100;
#if defined(_WIN32)
    bl.blBankTransitionMs = 100;         // Windows/WMS preset (approved 2026-08-25)
#else
    bl.blBankTransitionMs = 1000;        // macOS preset
#endif
    bl.postDelayMs = 3000;
    bl.blBankProbe = false;
    bl.blProbeFirst = false;
    bl.blProbeOnly = false;
    bl.blVerify = true;
    const int blResult = runBlSend(bl);

    if (blResult != 0)
    {
        std::cout << "\nFAILURE: Unfortunately your device failed to respond with the correct bootloader"
                     " version. Try power cycling your device, if it still boots and shows LED activity,"
                     " try the process again. If it does not boot, submit a support ticket to"
                     " https://support.musekinetics.com and include the firmware image and log file"
                     " located at: " << outDir << "\n";
        return 1;
    }

    std::cout << "\nSUCCESS: The bootloader was successfully installed.\n\n";

    // 6) Offer to load the latest application firmware via --fw-update.
    std::string latest;
    {
        deviceDatabase db;
        if (db.loadFamily(fam))
            db.getDefaultFirmwareVersion(latest);
    }
    std::cout << "Do you want to load the latest firmware version "
              << (latest.empty() ? std::string("(latest)") : latest) << " y/N: " << std::flush;
    std::string ans;
    std::getline(std::cin, ans);
    log.logInput(ans);
    const std::string a = trimUpper(ans);
    if (a == "Y" || a == "YES")
    {
        std::cout << "\nLoading firmware " << (latest.empty() ? std::string("(latest)") : latest) << " ...\n";
        CliOptions fw = options;
        fw.bootloaderInstall = false;
        fw.automaticMode = true;
        fw.familyName = "SoftStep"; // version defaults to the family's latest
        return runAutomaticProcess(fw);
    }
    std::cout << "Done. Firmware not loaded (bootloader is installed; you can run --fw-update SoftStep "
                 "any time).\n";
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

    // Declared here (not inside an if) so it stays alive for whichever run*()
    // branch below actually executes - see TimestampStreambuf's comment for
    // why timestamping every line, including \r progress-bar redraws, is
    // what's needed to diagnose real-hardware stall durations.
    TimestampStreambuf timestampBuf(std::cout.rdbuf());
    if (options.timestampOutput)
        std::cout.rdbuf(&timestampBuf);

    std::string midiBackendError;
    if (!midiBackend::initialize(options.midiBackendOverride, midiBackendError))
    {
        std::cout << "ERROR: " << midiBackendError << "\n";
        return 1;
    }

    std::cout << "OS: " << midiBackend::describeOs() << "\n";
    std::cout << "MIDI backend: " << midiBackend::describeSelectedApi() << "\n";

    std::cout << "Chunk size: " << options.chunkSize << " bytes, delay: " << options.chunkDelayMs
              << " ms, post-delay: " << options.postDelayMs << " ms\n";

    if (options.listNormalizedOnly && options.filePath.empty() && options.versionText.empty())
        return runManualProcess(options);

    if (options.bootloaderInstall)
        return runBootloaderInstall(options);

    if (options.idRequestOnly)
        return runIdentityRequest(options);

    if (options.blEraseRebootCmd)
        return runBlEraseRebootCmd(options);

    if (options.blDecodeOnly)
        return runBlDecode(options);

    if (options.blSend)
        return runBlSend(options);

    if (options.automaticMode)
        return runAutomaticProcess(options);

    return runManualProcess(options);
}