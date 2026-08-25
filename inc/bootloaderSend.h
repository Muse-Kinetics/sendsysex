#ifndef BOOTLOADER_SEND_H
#define BOOTLOADER_SEND_H

//
//  bootloaderSend.h
//
//  Sends a decoded LEGACY firmware image (see bootloaderUpgrade.h) to an
//  already-open RtMidiOut as a SINGLE F0..F7 message, one sector at a time,
//  with caller-controlled wall-clock gaps in place of the image's baked-in
//  zero padding. Because the receiver resets its stage flag on every F0, the
//  whole transfer stays inside one open message - individual sectors are sent
//  as successive MIDI writes (fragments of the one sysex), never their own
//  F0..F7. This is the same "many host-side writes, one logical message"
//  mechanism the chunked sub-split path already relies on.
//
//  Two drive modes:
//    * timed - each gap is a fixed sleep; runs unattended.
//    * step  - pause for the operator at each sector (Enter = next, a run of
//              digits = send that many, 'a' = auto-run the rest, 'q' = quit),
//              for watching a device's behaviour sector-by-sector at the bench.
//
//  SPDX-License-Identifier: MIT
//

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "RtMidi.h"
#include "bootloaderUpgrade.h"
#include "chunkedSysExTransfer.h" // findMatchingInputPort, ChunkReplyState, chunkReplyCallback
#include "MIDI_sysex.hpp"         // SysExMessageTX (build the REQUEST_FW_VERSION packet)

struct BlSendOptions
{
    bool step = false;               // interactive step mode
    unsigned int blockToDataGapMs = 40;   // gap after FW_BLOCK_HEADER (sector erase time)
    unsigned int sectorGapMs = 40;        // gap after FW_DATA (write/settle)
    unsigned int firstBlockToDataGapMs = 0; // override for the very first sector (0 = use blockToDataGapMs)
    unsigned int bankTransitionMs = 3000; // blind gap at the bank 0 -> 1 transition (used when bankProbe is off)
    unsigned int postDelayMs = 2000;      // after the final F7, before returning
    bool dryRun = false;              // print the schedule, send nothing

    // Fraction (0.0 .. 1.0) of the image's baked-in inter-packet padding to keep.
    //   1.0 (default) = keep ALL padding -> bytes on the wire are identical to the
    //                   source file (the exact, hardware-validated stream); the
    //                   gaps below are ADDED on top of that native padding.
    //   0.0           = drop all padding (the old --strip-padding behavior);
    //                   pace purely by the gaps. Drops ~19.7 KB, which on real
    //                   hardware misframes and reboots the unit - experiment only.
    //   between       = keep that fraction of each packet's trailing padding (a
    //                   prefix of the real bytes), to explore how much of the
    //                   baked-in pacing the added gaps can replace.
    // At 1.0 the byte-exact Raw spans are sent verbatim (the terminating F7 rides
    // in the final sector's dataRaw); at anything < 1.0 each packet is sent as its
    // own bytes + a scaled prefix of its padding, and the closing F7 is synthesized.
    double paddingScale = 1.0;

    // Bank-transition probe-and-wait: instead of a blind bankTransitionMs
    // delay, wait preProbeDelayMs (so the staged firmware finishes its
    // reboot/re-init deaf window), then send a bare REQUEST_FW_VERSION packet
    // (inside the still-open message, no new F0) and wait for the device's
    // version reply before streaming bank 1 - positive confirmation the stage
    // 1->2 handoff completed. Must NOT be used mid-bank-0: send_fw_version
    // clears fw_update_in_progress, which the staged firmware needs == 1 to set
    // second_stage; only safe once past the transition jump. Requires the
    // matching MIDI input port.
    bool bankProbe = false;
    unsigned int preProbeDelayMs = 250;    // wait after bank 0's last sector BEFORE the first probe
    unsigned int bankProbeTimeoutMs = 12000; // total budget to get a reply
    // The staged bank-0 firmware's send_fw_version() is byte-identical to the v93
    // app's - it computes the app CRC (crc_code_buf) before replying, so the reply
    // lands ~1.35s after the request. Resend must exceed that: a resend landing mid
    // CRC-compute stacks another request and backs the 8051 up, delaying/dropping
    // the answer (observed on real v93). 500ms was way too aggressive.
    unsigned int bankProbeResendMs = 2500;

    // Pre-send connectivity probe: before opening the update message, send a
    // device-specific REQUEST_FW_VERSION message to the matching input port and
    // require a reply within probeTimeoutMs; abort the whole send if none arrives,
    // so a disconnected/unresponsive device is caught before any firmware bytes go
    // out. The legacy v93 app does NOT implement the Universal Device Inquiry
    // (F0 7E 7F 06 01 F7) at all - it only dispatches KMI packets - so the probe
    // must send the same version request the editor uses (v93 fwupdate.c
    // send_version_request(): KMI header + ~50 zero pad + REQUEST_FW_VERSION packet
    // + F7); the app answers with a FW_HEADER (send_fw_version()). The header is
    // reused from the decoded image, so it self-adapts to the family being sent.
    // Requires the matching MIDI input port.
    bool probeFirst = false;
    bool probeOnly = false; // probe, report, and STOP before sending any firmware (safe poll)
    // The v93 reply takes ~1.3s: send_fw_header() computes the app CRC over the whole
    // image on the 8051 before the reply's tail + F7 go out, and RtMidi only delivers a
    // reassembled SysEx at F7 - so the timeout must comfortably exceed that.
    unsigned int probeTimeoutMs = 4000;
    // One request suffices and the reply lands ~1.35s later; resend only if it's
    // clearly been dropped, NOT while the device is still computing the reply -
    // an overlapping request backs the 8051 up and delays/loses the answer.
    unsigned int probeResendMs = 2500;
};

namespace blsend_detail
{

inline void sendBytes(RtMidiOut &out, const std::vector<uint8_t> &bytes, bool dryRun)
{
    if (dryRun || bytes.empty())
        return;
    // RtMidi wants a non-const vector<unsigned char>.
    std::vector<unsigned char> msg(bytes.begin(), bytes.end());
    out.sendMessage(&msg);
    // Block until this chunk is actually delivered, so the timing gaps that
    // follow (block-data / sector / bank) become REAL inter-message wire gaps
    // instead of being absorbed by an asynchronous send queue (CoreMIDI's
    // MIDISendSysex fires-and-returns). No-op on backends whose sendMessage
    // already blocks until sent (e.g. current WinMM).
    out.drain();
}

inline void napMs(unsigned int ms)
{
    if (ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// \r-updating sector progress bar, styled to match printProgress() (deviceHelpers.h)
// but keyed on sectors rather than bytes. Used by the timed (non-step) send so a
// long transfer shows [current/total] like the raw -f / --fw-update paths do.
// Prints a trailing newline once the last sector is sent.
inline void printSectorProgress(std::size_t sectorsSent, std::size_t totalSectors,
                                unsigned int bank, unsigned int blockNum,
                                const std::chrono::steady_clock::time_point &startTime)
{
    const int barWidth = 30;
    double progress = totalSectors > 0
                          ? static_cast<double>(sectorsSent) / static_cast<double>(totalSectors)
                          : 1.0;
    if (progress > 1.0)
        progress = 1.0;
    int filled = static_cast<int>(progress * barWidth + 0.5);
    if (filled > barWidth)
        filled = barWidth;

    const std::size_t remaining = totalSectors > sectorsSent ? totalSectors - sectorsSent : 0;
    const double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - startTime)
                               .count() /
                           1000.0;
    const double eta = (sectorsSent > 0 && progress > 0.0)
                           ? (elapsed / static_cast<double>(sectorsSent)) * static_cast<double>(remaining)
                           : 0.0;

    std::cout << '\r'
              << '[' << std::string(filled, '#') << std::string(barWidth - filled, '-') << "] "
              << "sector " << std::setw(3) << sectorsSent << '/' << std::setw(3) << totalSectors << "  "
              << "bank " << bank << " block " << std::setw(3) << blockNum << "  "
              << "elapsed " << formatDuration(elapsed) << "  "
              << "eta " << formatDuration(eta)
              << std::flush;

    if (remaining == 0)
        std::cout << "\n";
}

// Build a bare KMI REQUEST_FW_VERSION packet (category 0, type 0, no payload):
// 0x01 + 7bit-encoded {0, 0, length=0, crc}. NO F0/F7 - it is inserted inside
// the already-open update message, so it must not restart the receiver's
// header/stage state. Encoded via MIDI_CPP's TX so the CRC matches (an all-zero
// preamble's CRC is identical under the unsigned and legacy-signed variants).
inline std::vector<uint8_t> buildRequestFwVersionPacket()
{
    SysExMessageTX tx; // no send callback -> bytes stay in the buffer
    tx.clear();
    tx.single(SX_PACKET_START); // 0x01, raw
    tx.init_encode();
    tx.init_crc();
    tx.encode_crc_char(0x00);   // category
    tx.encode_crc_char(0x00);   // type = BLOCK_TYPE_REQUEST_FW_VERSION
    tx.encode_crc_int(0x0000);  // length field: 0 = no payload
    tx.encode_int(tx.getCRC()); // preamble CRC (itself not CRC'd)
    tx.flush_encode();
    return std::vector<uint8_t>(tx.getData(), tx.getData() + tx.getSize());
}

// Build a complete standalone REQUEST_FW_VERSION message for the PRE-SEND probe -
// what the legacy app (and the editor) expect. This reproduces v93's
// send_version_request() (Softstep2/fwupdate.c): the image's own outer header
// (F0 + KMI standard header + its ~50-byte zero pad) followed by a
// REQUEST_FW_VERSION packet, closed with F7. The running v93 app has NO Universal
// Device Inquiry handler - it only dispatches KMI packets - so this KMI request is
// the only thing it answers (with a FW_HEADER reply). The header/pad is taken from
// the decoded image (outerHeaderRaw), so the request self-adapts to whichever
// family's image is loaded, and the pad guarantees the receiver reads a full
// header before finding our packet's 0x01. Falls back to synthesizing pad if the
// image's outer header is unexpectedly short.
inline std::vector<uint8_t> buildLegacyVersionRequestMessage(const blupgrade::LegacyFirmwareImage &img)
{
    std::vector<uint8_t> msg = img.outerHeaderRaw.empty() ? img.outerHeaderWire : img.outerHeaderRaw;
    // Ensure enough bytes precede the packet that the receiver's fixed-size header
    // read completes and a packet-start search still has padding to scan.
    while (msg.size() < 16)
        msg.push_back(0x00);
    const std::vector<uint8_t> pkt = buildRequestFwVersionPacket();
    msg.insert(msg.end(), pkt.begin(), pkt.end());
    msg.push_back(0xF7);
    return msg;
}

// Reply state for the pre-send probe. Unlike chunkReplyCallback (which fires on any
// inbound F0), this matches the device's manufacturer signature anywhere in the
// message, so unrelated MIDI traffic on other ports doesn't count as a reply.
struct VersionReplyState
{
    volatile bool received;
    std::vector<unsigned char> sig; // e.g. {1B,48,7A,01} = mfg+product from the image header
    VersionReplyState() : received(false) {}
};

inline void versionReplyCallback(double /*ts*/, std::vector<unsigned char> *msg, void *userData)
{
    VersionReplyState *s = static_cast<VersionReplyState *>(userData);
    if (msg == 0 || msg->empty())
        return;
    if (s->sig.empty())
    {
        if ((*msg)[0] == 0xF0)
            s->received = true;
        return;
    }
    if (msg->size() < s->sig.size())
        return;
    for (std::size_t i = 0; i + s->sig.size() <= msg->size(); ++i)
    {
        bool match = true;
        for (std::size_t j = 0; j < s->sig.size(); ++j)
            if ((*msg)[i + j] != s->sig[j]) { match = false; break; }
        if (match) { s->received = true; return; }
    }
}

// Pre-send connectivity probe. Validated on real v93 hardware (2026-08-21): the
// reply comes back on the SAME port the request was sent to, but ~1.35s LATER,
// because send_fw_version() computes the app CRC over the whole image on the 8051
// before the reply's tail + F7 go out, and RtMidi only delivers a reassembled
// SysEx at F7. So the timeout MUST exceed ~1.5s (default probeTimeoutMs is 4000).
// We open EVERY input port (cheap, and defensive against a device that ever
// answers on a sibling port) and match the device's manufacturer signature
// (image header bytes 2..5, e.g. 1B 48 7A 01 - the same match the editor uses via
// _fw_reply_softstep) on any of them. Returns true on a reply; fills err on failure.
inline bool preSendVersionProbe(RtMidiOut &out, const std::string &portName,
                                const blupgrade::LegacyFirmwareImage &img,
                                const BlSendOptions &opt, std::string &err)
{
    VersionReplyState state;
    if (img.outerHeaderWire.size() >= 6)
        state.sig.assign(img.outerHeaderWire.begin() + 2, img.outerHeaderWire.begin() + 6);

    std::vector<RtMidiIn *> ins;
    unsigned int nIn = 0;
    try { RtMidiIn probe; nIn = probe.getPortCount(); } catch (...) { nIn = 0; }
    for (unsigned int i = 0; i < nIn; ++i)
    {
        RtMidiIn *in = 0;
        try
        {
            in = new RtMidiIn();
            in->ignoreTypes(false, false, false);
            in->openPort(i);
            in->setCallback(versionReplyCallback, &state);
            ins.push_back(in);
        }
        catch (...)
        {
            delete in;
        }
    }
    if (ins.empty())
    {
        err = "Pre-send probe (--probe) could not open any MIDI input port to listen for a reply.";
        return false;
    }

    std::cout << "Probing \"" << portName << "\" with a firmware-version request (listening on "
              << ins.size() << " input port(s), up to " << opt.probeTimeoutMs << "ms) ...\n";

    const std::vector<uint8_t> reqBytes = buildLegacyVersionRequestMessage(img);
    std::vector<unsigned char> msg(reqBytes.begin(), reqBytes.end());
    const unsigned int pollMs = 5;
    const unsigned int resendMs = opt.probeResendMs ? opt.probeResendMs : 1;
    unsigned int waited = 0;
    unsigned int sinceSend = resendMs; // force an immediate first send
    bool ok = false;
    while (waited < opt.probeTimeoutMs)
    {
        if (sinceSend >= resendMs)
        {
            out.sendMessage(&msg);
            out.drain();
            sinceSend = 0;
        }
        napMs(pollMs);
        waited += pollMs;
        sinceSend += pollMs;
        if (state.received) { ok = true; break; }
    }

    for (std::size_t i = 0; i < ins.size(); ++i)
    {
        try { ins[i]->closePort(); } catch (...) {}
        delete ins[i];
    }

    if (!ok)
        err = "No reply to the pre-send firmware-version request on any of " +
              std::to_string(ins.size()) + " input port(s) - \"" + portName +
              "\" is not responding; aborting before any firmware is sent.";
    return ok;
}

// Wait after the transition, then repeatedly send the REQUEST_FW_VERSION packet
// until the device answers (any inbound F0) or the budget expires. Returns true
// on a reply. The pre-probe delay lets the staged firmware finish its reboot/
// re-init deaf window before we bother probing.
inline bool probeBankTransition(RtMidiOut &out, ChunkReplyState &replyState,
                                const std::vector<uint8_t> &requestPacket,
                                const BlSendOptions &opt)
{
    std::cout << "  ... bank transition: waiting " << opt.preProbeDelayMs
              << "ms for staged firmware to reboot, then probing with REQUEST_FW_VERSION ...\n";
    napMs(opt.preProbeDelayMs);

    std::vector<unsigned char> pkt(requestPacket.begin(), requestPacket.end());
    const unsigned int pollMs = 5;
    unsigned int waited = 0;
    unsigned int sinceSend = opt.bankProbeResendMs; // force an immediate first send
    replyState.received = false;

    while (waited < opt.bankProbeTimeoutMs)
    {
        if (sinceSend >= opt.bankProbeResendMs)
        {
            out.sendMessage(&pkt);
            out.drain(); // ensure the request is actually on the wire before waiting for the reply
            sinceSend = 0;
        }
        napMs(pollMs);
        waited += pollMs;
        sinceSend += pollMs;
        if (replyState.received)
        {
            std::cout << "  ... probe reply received after " << waited
                      << "ms - staged firmware is live; sending bank 1.\n";
            return true;
        }
    }
    std::cout << "  ... NO probe reply within " << opt.bankProbeTimeoutMs << "ms.\n";
    return false;
}

// Returns how many more sectors to auto-send without prompting. In step mode
// this blocks for operator input; the return value lets a "send N" / "auto"
// command skip prompting for subsequent sectors.
inline long promptStep(const std::string &label)
{
    std::cout << label << "  [Enter=send next / <N>=send N / a=auto-run rest / q=quit] > "
              << std::flush;
    std::string line;
    if (!std::getline(std::cin, line))
        return 0; // EOF -> stop
    // trim
    std::size_t b = line.find_first_not_of(" \t\r\n");
    std::size_t e = line.find_last_not_of(" \t\r\n");
    std::string tok = (b == std::string::npos) ? std::string() : line.substr(b, e - b + 1);

    if (tok == "q" || tok == "Q")
        return -1; // quit sentinel
    if (tok == "a" || tok == "A")
        return 1L << 30; // effectively "rest"
    if (tok.empty())
        return 1;
    // a bare count
    char *end = 0;
    long n = std::strtol(tok.c_str(), &end, 10);
    if (end && *end == '\0' && n > 0)
        return n;
    return 1; // anything else: just advance one
}

} // namespace blsend_detail

// Streams the whole image inside one F0..F7. Returns false (and fills err) on a
// transport error or operator quit before completion.
inline bool sendLegacyImageSectorwise(RtMidiOut &out,
                                       const std::string &portName,
                                       const blupgrade::LegacyFirmwareImage &img,
                                       const BlSendOptions &opt,
                                       std::string &err)
{
    using namespace blsend_detail;

    if (!img.ok)
    {
        err = "Image not decoded: " + img.error;
        return false;
    }

    // Full padding (>= 1.0) sends the byte-exact Raw spans verbatim; the closing
    // F7 rides in the final sector's dataRaw. Anything less sends each packet's
    // own bytes plus a scaled prefix of its trailing padding, and the F7 is
    // synthesized at the end. finalSpan trims the terminating F7 off the last
    // span so the scaled padding lands before it, not after.
    const bool keepFullPadding = (opt.paddingScale >= 1.0);
    const double pScale = (opt.paddingScale < 0.0) ? 0.0 : opt.paddingScale;
    auto span = [&](const std::vector<uint8_t> &raw, std::size_t wireLen,
                    bool finalSpan) -> std::vector<uint8_t> {
        if (keepFullPadding)
            return raw;
        const std::size_t rawLen = raw.size();
        std::size_t padEnd = finalSpan ? (rawLen ? rawLen - 1 : 0) : rawLen; // drop trailing F7
        if (padEnd < wireLen)
            padEnd = wireLen; // safety: never cut into the packet's own bytes
        const std::size_t padLen = padEnd - wireLen;
        std::size_t keep = static_cast<std::size_t>(std::llround(static_cast<double>(padLen) * pScale));
        if (keep > padLen)
            keep = padLen;
        return std::vector<uint8_t>(raw.begin(), raw.begin() + wireLen + keep);
    };

    const std::size_t totalSectors = img.totalSectors();
    std::cout << "Legacy sector-wise send to \"" << portName << "\": "
              << img.banks.size() << " bank(s), " << totalSectors << " sector(s). "
              << (opt.dryRun ? "(DRY RUN - nothing sent)\n" : "")
              << "Mode: " << (opt.step ? "STEP" : "TIMED")
              << "  padding=" << (opt.paddingScale >= 1.0
                                      ? "FULL (byte-exact)"
                                      : (opt.paddingScale <= 0.0
                                             ? "STRIPPED"
                                             : "scaled x" + std::to_string(opt.paddingScale)))
              << "  blockToDataGap=" << opt.blockToDataGapMs << "ms"
              << " sectorGap=" << opt.sectorGapMs << "ms"
              << " bankTransition=" << opt.bankTransitionMs << "ms"
              << " postDelay=" << opt.postDelayMs << "ms\n";

    // Pre-send connectivity probe (self-contained: opens/closes its own listeners
    // across ALL input ports, because the legacy reply comes back on a sibling port
    // - see preSendVersionProbe). Abort the whole send if the device stays silent.
    if (opt.probeFirst && !opt.dryRun)
    {
        if (!preSendVersionProbe(out, portName, img, opt, err))
            return false;
        if (opt.probeOnly)
        {
            std::cout << "  ... device responded. Probe-only: NOT sending firmware.\n";
            return true;
        }
        std::cout << "  ... device responded; proceeding with the firmware send.\n";
    }

    // A matching input port is needed for the bank-transition probe (--bank-probe);
    // open it up front so a mid-transfer sync failure isn't discovered too late.
    RtMidiIn midiIn;
    ChunkReplyState replyState;
    std::vector<uint8_t> requestPacket;
    bool haveInput = false;
    const bool wantBankProbe = opt.bankProbe && img.banks.size() > 1;
    if (!opt.dryRun && wantBankProbe)
    {
        midiIn.ignoreTypes(false, false, false);
        const int inPort = findMatchingInputPort(midiIn, portName);
        if (inPort < 0)
        {
            err = "Bank-transition probe (--bank-probe) needs a MIDI input port matching \"" +
                  portName + "\", but none was found.";
            return false;
        }
        midiIn.setCallback(chunkReplyCallback, &replyState);
        midiIn.openPort(static_cast<unsigned int>(inPort));
        haveInput = true;
        requestPacket = buildRequestFwVersionPacket();
    }

    // Open the single message with the outer header. In keep-padding mode the
    // Raw spans (which include the file's inter-packet padding) tile the file
    // exactly, so sending them reproduces the byte-for-byte original; the closing
    // F7 rides in the final sector's dataRaw and must NOT be re-synthesized.
    try
    {
        sendBytes(out, span(img.outerHeaderRaw, img.outerHeaderWire.size(), false), opt.dryRun);
    }
    catch (RtMidiError &e)
    {
        if (haveInput) midiIn.closePort();
        err = e.getMessage();
        return false;
    }

    long autoRemaining = 0; // sectors to send without prompting (step mode)
    std::size_t sentSectors = 0;
    bool firstSector = true;
    const bool showProgress = !opt.step && !opt.dryRun; // step prints its own prompts

    for (std::size_t bi = 0; bi < img.banks.size(); ++bi)
    {
        const blupgrade::Bank &bank = img.banks[bi];
        std::cout << "\n-- BANK " << static_cast<unsigned>(bank.bank)
                  << " (build " << bank.buildNumber << ", " << bank.sectors.size()
                  << " sectors) --\n";

        try
        {
            sendBytes(out, span(bank.headerRaw, bank.headerWire.size(), false), opt.dryRun); // FW_HEADER
        }
        catch (RtMidiError &e)
        {
            err = e.getMessage();
            return false;
        }

        // Each bank gets its own [current/total] progress bar and timing.
        std::size_t sectorInBank = 0;
        const auto bankStartTime = std::chrono::steady_clock::now();

        for (std::size_t si = 0; si < bank.sectors.size(); ++si)
        {
            const blupgrade::Sector &sec = bank.sectors[si];

            if (opt.step && autoRemaining <= 0)
            {
                char lbl[96];
                std::snprintf(lbl, sizeof(lbl),
                              "  sector %zu/%zu  bank %u block %u (%u bytes)",
                              sentSectors + 1, totalSectors,
                              static_cast<unsigned>(bank.bank),
                              static_cast<unsigned>(sec.blockNum), sec.dataLength);
                long n = promptStep(lbl);
                if (n < 0)
                {
                    err = "Aborted by operator before completion (device left mid-transfer).";
                    return false;
                }
                autoRemaining = n;
            }

            const unsigned int b2d = (firstSector && opt.firstBlockToDataGapMs)
                                         ? opt.firstBlockToDataGapMs
                                         : opt.blockToDataGapMs;
            const bool isFinalSpan = (bi + 1 == img.banks.size()) &&
                                     (si + 1 == bank.sectors.size());
            try
            {
                sendBytes(out, span(sec.blockHeaderRaw, sec.blockHeaderWire.size(), false),
                          opt.dryRun); // triggers sector erase
                napMs(b2d);
                sendBytes(out, span(sec.dataRaw, sec.dataWire.size(), isFinalSpan),
                          opt.dryRun); // writes the sector
            }
            catch (RtMidiError &e)
            {
                err = e.getMessage();
                return false;
            }

            ++sentSectors;
            ++sectorInBank;
            firstSector = false;
            if (showProgress)
                printSectorProgress(sectorInBank, bank.sectors.size(),
                                    static_cast<unsigned>(bank.bank),
                                    static_cast<unsigned>(sec.blockNum), bankStartTime);
            if (opt.step && autoRemaining > 0 && autoRemaining < (1L << 30))
                --autoRemaining;

            const bool lastSectorOfBank = (si + 1 == bank.sectors.size());
            const bool lastBank = (bi + 1 == img.banks.size());
            if (lastSectorOfBank && !lastBank)
            {
                if (wantBankProbe && haveInput)
                {
                    if (!probeBankTransition(out, replyState, requestPacket, opt))
                    {
                        midiIn.closePort();
                        err = "No REQUEST_FW_VERSION reply at the bank transition - the staged "
                              "firmware did not come up; aborting before bank 1 (device left "
                              "mid-transfer).";
                        return false;
                    }
                }
                else
                {
                    std::cout << "  ... bank transition: waiting " << opt.bankTransitionMs
                              << "ms for staged firmware to reboot (blind) ...\n";
                    napMs(opt.bankTransitionMs);
                }
            }
            else
            {
                napMs(opt.sectorGapMs);
            }
        }
    }

    // Close the single message. At full padding the terminating F7 (and any
    // trailing padding) already went out as the tail of the final sector's dataRaw,
    // so re-sending one here would append a second, stray F7 - skip it. When
    // padding is scaled down that final F7 was trimmed off, so synthesize it here.
    if (!keepFullPadding)
    {
        try
        {
            std::vector<uint8_t> stop(1, 0xF7);
            sendBytes(out, stop, opt.dryRun);
        }
        catch (RtMidiError &e)
        {
            if (haveInput) midiIn.closePort();
            err = e.getMessage();
            return false;
        }
    }

    if (haveInput)
        midiIn.closePort();

    std::cout << "\nSent " << sentSectors << " sector(s); closed message (F7). "
              << "Waiting postDelay " << opt.postDelayMs << "ms.\n";
    napMs(opt.postDelayMs);
    return true;
}

#endif // BOOTLOADER_SEND_H
