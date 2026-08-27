#ifndef CHUNKED_SYSEX_TRANSFER_H
#define CHUNKED_SYSEX_TRANSFER_H

//
//  chunkedSysExTransfer.h
//
//  Sends a Hex_to_SysEx-chunked (or legacy single-message) .syx byte buffer
//  out an already-open RtMidiOut, with a between-chunk identity-request
//  handshake so a partial/corrupted transfer aborts loudly instead of
//  silently sending a broken firmware image.
//
//  Shared by the raw manual send path (SendSysEx.cpp, -p/-n -f) and the
//  automatic-update path (kmiDevice.cpp, --fw-update) so both speak the
//  identical, hardware-validated protocol rather than two independent
//  reimplementations drifting apart. See kboard-perf-test-design.md and
//  mk_firmware_tester's decisions.md (2026-07-16 entries) for the
//  reliability fixes baked in here and why they were needed.
//
//  Per-chunk reply handshake (2026-08-25 redesign): --chunk-delay is pure
//  inter-chunk pacing (a minimum wait after sending a chunk, before asking
//  for its reply); --id-reply-timeout is how long one identity-request
//  attempt waits for a reply; --id-reply-resend-attempts is how many times
//  a silent chunk gets re-asked (each a fresh --id-reply-timeout-long wait)
//  before the chunk is declared lost. If every attempt comes back empty,
//  the whole file is resent from chunk 0 (SysEx can't resume mid-message) -
//  that outer retry is unchanged and lives in sendChunkedFileWithRetry
//  below. Previously a single chunkDelayMs value did all three jobs at
//  once (pacing, budget, AND a fixed 300ms internal resend cadence),
//  which meant tuning any one of them for a specific device (see QuNeo's
//  real-hardware failures, 2026-08-25) distorted the others.
//
//  SPDX-License-Identifier: MIT
//

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "RtMidi.h"
#include "deviceHelpers.h"
#include "sysExChunking.h"

// WinMM assigns a trailing disambiguation index to bare-alias port names
// independently per port-direction list, so the same physical device can
// enumerate as e.g. input "K-Board 4" / output "K-Board 5" - same device,
// different OS-assigned suffix. Strip a trailing " <digits>" token so
// input/output names for the same device compare equal.
//
// Windows (WinMM) only: on other backends (CoreMIDI, ALSA) a multi-port
// device's ports are already stably numbered as part of the name itself
// (e.g. "Mimic Hub MIDI Port 1".."15") - stripping the trailing digit there
// collapses every port to the same base string and causes false-positive
// pairing collisions. Identity function on non-Windows builds so the
// fallback comparison degenerates to the exact-match check and can never
// introduce a mismatch.
inline std::string stripTrailingIndex(const std::string &name)
{
#if defined(_WIN32)
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
#else
    return name;
#endif
}

inline int findMatchingInputPort(RtMidiIn &midiIn, const std::string &outputPortName)
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

struct ChunkReplyState
{
    volatile bool received;
    ChunkReplyState() : received(false) {}
};

inline void chunkReplyCallback(double /*timeStamp*/, std::vector<unsigned char> *message, void *userData)
{
    ChunkReplyState *state = static_cast<ChunkReplyState *>(userData);
    if (message != 0 && !message->empty() && (*message)[0] == 0xF0)
        state->received = true;
}

// Sends a Universal Non-Realtime Device Inquiry (F0 7E 7F 06 01 F7) once and
// waits up to waitMs for any SysEx reply (delivered via chunkReplyCallback).
// Exactly one request goes out per call - no internal resend. Retrying a
// no-reply chunk (send another request, wait again) is the caller's job now
// (see the attempt loop in sendChunkedFileToPort), driven by explicit,
// separately-tunable --id-reply-timeout / --id-reply-resend-attempts values
// instead of a fixed, baked-in 300ms resend cadence folded into a single
// opaque wait - real QuNeo hardware showed the old single "chunkDelayMs
// doubles as the whole reply-wait-plus-resend budget" design made it
// impossible to raise the retry count without also stretching (or shrinking)
// every healthy chunk's steady-state pacing, and vice versa (2026-08-25).
inline bool waitForIdReplyOnce(RtMidiOut &midiOut, ChunkReplyState &state, unsigned int waitMs)
{
    static const std::vector<unsigned char> idRequest = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    state.received = false;

    const unsigned int pollIntervalMs = 5;

    // Real-hardware finding (2026-08-26, SoftStep, WMS): elapsed time used to
    // be a naive counter incremented by a fixed pollIntervalMs per loop
    // iteration, on the assumption each iteration costs exactly that much
    // wall-clock time. It doesn't - midiOut.sendMessage() itself can block
    // for far longer than 5ms (observed: a 3000ms-budget wait actually took
    // ~9.4s wall-clock, --timestamp made this visible), and that extra time
    // was invisible to the counter, silently blowing well past the
    // configured budget. Measuring against steady_clock instead makes the
    // actual elapsed time match what was asked for, regardless of how long
    // the sendMessage() call itself takes.
    //
    // Regression fixed 2026-08-26 (SoftStep, CoreMIDI): drain() after the send,
    // then sample 'start'. On CoreMIDI sendMessage() is asynchronous - it queues
    // the request behind any still-in-flight data chunk and returns immediately,
    // so the request can sit in CoreMIDI's queue for far longer than waitMs
    // before it actually reaches the wire. Sampling 'start' right after the
    // (async) send therefore starts the reply clock while the request is still
    // queued: the whole budget can elapse before the request is even out, the
    // device then replies within a few ms of it finally leaving (confirmed on
    // the wire via MIDI Monitor), but we've already given up ("NO REPLY within
    // 100 ms" on a chunk the board answered promptly). drain() blocks until the
    // request is genuinely on the wire, so waitMs then measures a real reply
    // window. On Windows drain() is a no-op (MidiOutWinMM/WMS inherit the base
    // no-op), so this is a no-op there.
    midiOut.sendMessage(&idRequest);
    midiOut.drain();
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    while (!state.received)
    {
        const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - start).count();
        if (elapsedMs >= static_cast<long long>(waitMs))
            break;

        std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
    }
    return state.received;
}

// Sends the identity request and waits up to replyWaitMs for a reply, and if
// that first attempt gets nothing, resends and waits again - up to
// resendAttempts additional times (so resendAttempts=2 means 3 total
// requests: 1 initial + 2 resends). Each attempt is a fresh, independent
// waitForIdReplyOnce() call with its own full replyWaitMs budget. Returns
// true as soon as any attempt gets a reply.
//
// Logging is gated to the LAST TWO attempts only (plus the very first,
// silent-on-success check). With a generous resend count - e.g. QuNeo's
// real-hardware USB-FIFO race (2026-08-25 investigation) needing
// --id-reply-resend-attempts 6+ to reliably recover - most chunks fail their
// first attempt or two and recover on a routine resend; printing every one
// of those floods the log with expected, non-actionable noise (one 3-line
// block per recovered chunk, every few chunks, for a 295+ chunk transfer).
// Only a chunk that has burned through all but its last attempt or two -
// i.e. is genuinely at risk of the whole transfer aborting - gets logged,
// which is the situation an operator actually needs to see. Earlier silent
// attempts still count against resendAttempts as normal; only their output
// is suppressed.
inline bool waitForIdReplyWithResends(RtMidiOut &midiOut, ChunkReplyState &state,
                                      unsigned int replyWaitMs, unsigned int resendAttempts,
                                      std::size_t chunkNumber, std::size_t totalChunks)
{
    const unsigned int totalTries = resendAttempts + 1;
    const unsigned int firstLoggedAttempt = (totalTries > 2) ? (totalTries - 1) : 1;
    bool printedAnything = false;

    for (unsigned int attempt = 1; attempt <= totalTries; ++attempt)
    {
        const bool gotReply = waitForIdReplyOnce(midiOut, state, replyWaitMs);

        if (attempt == 1 && gotReply)
            return true; // silent happy path - caller prints "reply OK (N left)"

        const bool logThisAttempt = attempt >= firstLoggedAttempt;

        if (gotReply)
        {
            if (logThisAttempt)
                std::cout << (printedAnything ? "  " : "\n  ") << "chunk " << chunkNumber << "/" << totalChunks
                          << ": reply received on resend (attempt " << attempt << "/" << totalTries << ")\n";
            return true;
        }

        if (logThisAttempt)
        {
            std::cout << (printedAnything ? "  " : "\n  ") << "chunk " << chunkNumber << "/" << totalChunks
                      << ": " << (printedAnything ? "still " : "") << "NO REPLY within " << replyWaitMs
                      << " ms (attempt " << attempt << "/" << totalTries << ")\n";
            printedAnything = true;

            if (attempt < totalTries)
                std::cout << "  chunk " << chunkNumber << "/" << totalChunks
                          << ": resending id request (attempt " << (attempt + 1) << "/" << totalTries << ")...\n";
        }
    }
    return false;
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
// already >= every chunk's length (the normal, expected case), each chunk
// goes out as a single, whole, correctly-framed message. IMPORTANT: chunkSize
// smaller than an actual chunk's on-wire length (payload + hex-record header)
// causes that message to be split with no F0/F7 framing on the pieces, which
// is not a valid SysEx split and can corrupt that message - callers should
// pass a chunkSize comfortably at or above the packaging tool's chunk size,
// not try to cut it close (see k-board-firmware's commands.md, 2026-07-16).
// firstGapDelayMs, when nonzero, overrides chunkDelayMs for the single gap
// following the very first sub-split window of the first chunk only; every
// other gap (including all later chunks' sub-split windows) still uses
// chunkDelayMs. 0 disables the override entirely (every gap uses
// chunkDelayMs, matching this function's original behavior).
//
// firstChunkSize, when nonzero, overrides chunkSize for the very first
// sub-split window of the first chunk only; every later window (including
// the rest of the first chunk) still uses chunkSize. 0 disables the
// override entirely (every window uses chunkSize, matching this function's
// original behavior). Lets a small, fast-clearing first window (avoiding a
// mid-transmission stall - see firstGapDelayMs below) be paired with a
// larger, more efficient window size for the remainder of the transfer once
// the receiver's settled.
//
// Added for KBP4: the Central device's application-mode SysEx relay
// recognizes a valid message header (the packet's first ~24 bytes) as soon
// as it arrives and immediately starts a blocking operation on its end
// (observed: consistent with an I2C flash-erase on the peripheral MCU it's
// relaying to) before it can accept more USB data. A short first window
// (comfortably under the ~64-byte point where the transfer itself starts
// landing mid-blocking-op) clears the host fast enough to avoid that, but
// the *next* window still needs several seconds' grace before the device
// is ready again - after which it drains the rest of the transfer at
// normal chunkDelayMs speed and can use a larger chunkSize than the first
// window needed. See kbp4.json's payload notes and
// k-board_pro_firmware's .buddy-project/commands.md (2026-08-13) for the
// hardware investigation this came from.
inline bool sendChunkedFileToPort(RtMidiOut &midiOut,
                                  const std::string &outputPortName,
                                  const std::vector<unsigned char> &bytes,
                                  const std::vector<SysExChunk> &chunks,
                                  unsigned int chunkSize,
                                  unsigned int chunkDelayMs,
                                  unsigned int postDelayMs,
                                  std::string &errorMessage,
                                  unsigned int firstGapDelayMs = 0,
                                  unsigned int firstChunkSize = 0,
                                  bool finalChunkRebootsToApp = false,
                                  unsigned int idReplyTimeoutMs = 300,
                                  unsigned int idReplyResendAttempts = 2)
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
    ChunkReplyState replyState;
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

    std::size_t totalBytes = 0;
    for (std::size_t i = 0; i < chunks.size(); ++i)
        totalBytes += chunks[i].length;
    std::size_t bytesSentOverall = 0;
    const std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();

    bool ok = true;
    for (std::size_t i = 0; i < chunks.size() && ok; ++i)
    {
        const SysExChunk &chunk = chunks[i];

        try
        {
            // firstChunkSize only ever applies to the very first window of the
            // very first chunk (i == 0) - later chunks (multi-chunk Hex_to_SysEx
            // files) and later windows within this chunk always use chunkSize.
            const bool useFirstChunkSize = (i == 0 && firstChunkSize > 0 && firstChunkSize < chunk.length);
            if (chunkSize < chunk.length || useFirstChunkSize)
            {
                if (useFirstChunkSize)
                    std::cout << "  sub-split: first window " << firstChunkSize
                              << " bytes, then " << chunkSize << "-byte windows\n";
                else
                    std::cout << "  sub-split into " << chunkSize << "-byte windows (chunk size < message length)\n";
                std::size_t sent = 0;
                bool isFirstWindow = true;
                while (sent < chunk.length)
                {
                    const std::size_t windowSize = (isFirstWindow && useFirstChunkSize) ? firstChunkSize : chunkSize;
                    const std::size_t sizeToSend = std::min<std::size_t>(windowSize, chunk.length - sent);
                    std::vector<unsigned char> window(bytes.begin() + chunk.offset + sent,
                                                       bytes.begin() + chunk.offset + sent + sizeToSend);
                    midiOut.sendMessage(&window);
                    sent += sizeToSend;
                    bytesSentOverall += sizeToSend;
                    printProgress(bytesSentOverall, totalBytes, sizeToSend, startTime);

                    if (sent < chunk.length)
                    {
                        const unsigned int gapMs = (isFirstWindow && i == 0 && firstGapDelayMs > 0)
                                                  ? firstGapDelayMs
                                                  : chunkDelayMs;
                        if (gapMs > 0)
                            std::this_thread::sleep_for(std::chrono::milliseconds(gapMs));
                        isFirstWindow = false;
                    }
                }
            }
            else
            {
                std::vector<unsigned char> whole(bytes.begin() + chunk.offset,
                                                 bytes.begin() + chunk.offset + chunk.length);
                midiOut.sendMessage(&whole);
                bytesSentOverall += chunk.length;
                printProgress(bytesSentOverall, totalBytes, chunk.length, startTime);
            }
        }
        catch (RtMidiError &error)
        {
            if (bytesSentOverall < totalBytes)
                std::cout << "\n";
            errorMessage = error.getMessage();
            ok = false;
            break;
        }

        if (haveInput)
        {
            // Checked after every chunk, including the last - previously
            // skipped for the last chunk (isLastChunk special case, removed
            // 2026-08-18), on the assumption that a "Successfully sent all
            // N chunk(s)" transport-level message meant the transfer landed.
            // It didn't: a real K-Board update reported success this way
            // while the final EOF chunk had actually been dropped (same
            // class of receiver-busy race waitForIdReply already guards
            // every other chunk against), leaving the device stuck in its
            // bootloader with no error surfaced. The device stays
            // responsive to a generic identity inquiry whether it's still
            // in the bootloader (chunk lost) or has already rebooted into
            // the application (chunk landed), so this check distinguishes
            // the two cases the same way it does mid-transfer.
            //
            // Drain the just-sent data chunk onto the wire before doing
            // anything time-based. CoreMIDI's sendMessage() is asynchronous, so
            // without this the 50ms "settle" below would start while the chunk
            // is still draining (overlapping the transmission instead of
            // following it), and the id request would queue behind the chunk -
            // the exact backlog that made replies land outside the reply window
            // around chunk 6 on real SoftStep hardware. drain() bounds the queue
            // to one chunk at a time and makes the settle a real post-receipt
            // gap. No-op on Windows (WinMM/WMS inherit the base no-op drain()).
            midiOut.drain();

            // Give the device a moment to finish receiving/start processing
            // the chunk before demanding a reply - firing the ID request
            // immediately risks it landing while the chunk's tail end is
            // still being handled on the device side and getting missed.
            // This is -cd/--chunk-delay's whole job now: a minimum wait
            // between sending a chunk and demanding its reply (previously a
            // fixed, non-configurable 50ms - made tunable 2026-08-25 after
            // chunkDelayMs was split from the reply-wait budget below, since
            // some receivers settle faster or slower than 50ms and there was
            // no way to adjust just this without also changing how long a
            // reply was waited for).
            if (chunkDelayMs > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(chunkDelayMs));

            // The reply-wait budget (per attempt) also needs to match
            // whichever grace period this specific chunk's *send* used, not
            // always idReplyTimeoutMs - found 2026-08-18 from a real transfer
            // that failed at chunk 1 (used firstChunkSize/firstGapDelayMs
            // because the receiver needs it) and separately at the final
            // chunk, both with "NO REPLY" even though -fgd 3000/-pd 3000 had
            // been explicitly passed for exactly this reason. Chunk 0's
            // sub-split gap already waits firstGapDelayMs *between* its
            // windows for the same receiver behavior - the reply wait right
            // after it needs at least that much grace too, not the
            // steady-state --id-reply-timeout. The final chunk is the same
            // idea: postDelayMs exists specifically because the receiver may
            // need extra time (write/verify/reboot-prep) after the last
            // chunk before it can talk again.
            //
            // Bug fixed 2026-08-26 (real SoftStep hardware): this used to
            // additionally require firstChunkSize > 0 && firstChunkSize <
            // chunk.length - i.e. an actual sub-split of chunk 0 - before
            // applying firstGapDelayMs at all. But the receiver's blocking
            // erase is triggered by it recognizing chunk 0's header, which
            // happens regardless of whether the *sender* chose to sub-split
            // that chunk's own transmission. A family whose chunks are
            // already small pre-framed messages (SoftStep's Hex_to_SysEx
            // packaging - chunk 0 as small as 38 bytes) never needs
            // firstChunkSize sub-splitting at all, so that old condition was
            // always false and firstGapDelayMs from softstep.json's
            // firmwareUpdateDefaults was silently never applied.
            //
            // Split 2026-08-25 from a single chunkDelayMs doing triple duty
            // (inter-chunk pacing, reply-wait budget, AND a baked-in 300ms
            // resend cadence) into three independent knobs after real QuNeo
            // hardware showed that design couldn't be tuned for one purpose
            // without distorting the others: --chunk-delay is pure pacing
            // (above), --id-reply-timeout is this per-attempt reply budget,
            // and --id-reply-resend-attempts (used below) controls how many
            // times a silent chunk gets re-asked before the whole transfer
            // is considered failed and the coarse-grained retry
            // (sendChunkedFileWithRetry, resend-the-whole-file-from-chunk-0)
            // takes over - SysEx has no mid-message resume, so that outer
            // retry has always meant starting over, and still does.
            const bool isFirstChunk = (i == 0);
            const bool isLastChunk = (i + 1 == chunks.size());

            // Families whose device reboots straight into application mode as it
            // commits the final chunk (softstep.json's rebootsToAppOnFinalChunk)
            // never answer an identity request on the bootloader port afterwards
            // - the port is already gone. Skip the doomed final-chunk handshake
            // entirely and finish the send cleanly; runAutomaticUpdate then does
            // the real success test on the application port (reconnect + version
            // match). Waiting postDelayMs for a reply that structurally cannot
            // come, then burning a whole retry cycle before the app-port
            // confirmation, was pure latency (~9s on SoftStep). Non-final chunks
            // are unaffected: a dropped mid-transfer chunk still aborts here.
            if (isLastChunk && finalChunkRebootsToApp)
            {
                std::cout << "\n  final chunk sent; device reboots to application mode"
                             " - confirming there\n";
                continue;
            }

            unsigned int replyWaitMs = idReplyTimeoutMs;
            if (isFirstChunk)
                replyWaitMs = std::max(replyWaitMs, firstGapDelayMs);
            if (isLastChunk)
                replyWaitMs = std::max(replyWaitMs, postDelayMs);

            if (waitForIdReplyWithResends(midiOut, replyState, replyWaitMs, idReplyResendAttempts,
                                          i + 1, chunks.size()))
            {
                // Folded into the same in-place bar line (printProgress just
                // left the cursor right after it, mid-transfer) rather than a
                // permanent line per chunk - a many-small-chunk file (e.g.
                // SoftStep's ~290 chunks) would otherwise flood the terminal
                // with one scrollback line per chunk instead of one steadily
                // updating line. Padded to a fixed width so a shorter later
                // redraw can't leave stray trailing characters from a longer
                // earlier one. Real failures above still get permanent,
                // newline-broken lines (one per attempt) - that visibility is
                // what surfaced the 2026-08-25 SoftStep timing issue and must
                // not be lost.
                std::string suffix = "  reply OK (" + std::to_string(chunks.size() - i - 1) + " left)";
                if (suffix.size() < 24)
                    suffix.append(24 - suffix.size(), ' ');
                std::cout << suffix << std::flush;
            }
            else
            {
                errorMessage = "No identity reply after chunk " + std::to_string(i + 1) + "/"
                              + std::to_string(chunks.size()) + " (" + std::to_string(idReplyResendAttempts + 1)
                              + " attempt(s), " + std::to_string(replyWaitMs) + " ms each)"
                              + (isLastChunk
                                     ? " (the final chunk) - the device may not have received it and could"
                                       " still be sitting in its bootloader; aborting transfer."
                                     : " - aborting transfer.");
                ok = false;
            }
        }
    }

    if (haveInput)
        midiIn.closePort();

    if (ok)
    {
        // The last chunk's "reply OK" suffix (if any) is left mid-line by
        // design (see above) - break out of it before the summary line.
        if (haveInput)
            std::cout << "\n";
        std::cout << "Successfully sent all " << chunks.size() << " chunk(s) to MIDI OUT port: "
                  << outputPortName << ", size: " << bytes.size() << " bytes.\n";
        if (postDelayMs > 0U)
            std::this_thread::sleep_for(std::chrono::milliseconds(postDelayMs));
    }

    return ok;
}

// Retry parameters for sendChunkedFileWithRetry(). Originally
// kmiDevice::sendPayloadFileToPort()'s exclusively, added for WinMM
// rejecting the first sendMessage on a freshly opened port immediately
// after a device reboot (MMRESULT=1) even though midiOutOpen() itself
// reported success. Factored out here 2026-08-12 so the raw manual send
// path (-p/-n -f) gets the same protection - prompted by a
// MidiOutWinMM::sendMessage failure on real hardware when --midi-backend
// winmm forces WinMM on a machine actually running the WMS translation
// layer underneath it, which is flakier than native WinMM.
const int kChunkedSendRetryMaxAttempts = 3;
const int kChunkedSendRetryDelayMs = 3000;
const int kChunkedSendRetryPortSettleMs = 500;

// Retries a whole chunked-file transfer up to kChunkedSendRetryMaxAttempts
// times if it fails outright. SysEx cannot be resumed mid-message, so a
// retry always means "close, wait for the driver to release the handle,
// reopen, wait for it to settle, then resend the entire file from the top" -
// never a resumption of the failed attempt.
//
// openPort/closePort let each caller keep its own port-resolution semantics
// rather than this function dictating one: kmiDevice re-resolves by
// normalized name via its device database (openTransferOutputByName);
// SendSysEx.cpp's raw send re-resolves by the exact RtMidi port name already
// selected, with no normalization, consistent with raw send's existing
// documented behavior. openPort is called fresh on every attempt (including
// the first) and must return the now-open port to send through, or nullptr
// on failure - returning nullptr aborts immediately with no retry, matching
// the original kmiDevice behavior (an open failure is not treated as the
// same kind of transient condition a send failure is).
inline bool sendChunkedFileWithRetry(const std::string &portName,
                                     const std::vector<unsigned char> &bytes,
                                     const std::vector<SysExChunk> &chunks,
                                     unsigned int chunkSize,
                                     unsigned int chunkDelayMs,
                                     unsigned int postDelayMs,
                                     const std::function<RtMidiOut *()> &openPort,
                                     const std::function<void()> &closePort,
                                     std::string &errorMessage,
                                     unsigned int firstGapDelayMs = 0,
                                     unsigned int firstChunkSize = 0,
                                     bool finalChunkRebootsToApp = false,
                                     unsigned int idReplyTimeoutMs = 300,
                                     unsigned int idReplyResendAttempts = 2)
{
    for (int attempt = 0; attempt < kChunkedSendRetryMaxAttempts; ++attempt)
    {
        RtMidiOut *midiOut = openPort();
        if (midiOut == 0)
            return false;

        std::this_thread::sleep_for(std::chrono::milliseconds(kChunkedSendRetryPortSettleMs));

        const bool sent = sendChunkedFileToPort(*midiOut, portName, bytes, chunks,
                                                chunkSize, chunkDelayMs, postDelayMs, errorMessage,
                                                firstGapDelayMs, firstChunkSize, finalChunkRebootsToApp,
                                                idReplyTimeoutMs, idReplyResendAttempts);
        closePort();

        if (sent)
            return true;

        const bool isLastAttempt = (attempt + 1 == kChunkedSendRetryMaxAttempts);
        if (!isLastAttempt)
        {
            std::cout << "Send failed (" << errorMessage << "); retrying in "
                      << (kChunkedSendRetryDelayMs / 1000) << " second(s)... (attempt "
                      << (attempt + 2) << "/" << kChunkedSendRetryMaxAttempts << ")\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(kChunkedSendRetryDelayMs));
        }
    }

    return false;
}

#endif // CHUNKED_SYSEX_TRANSFER_H
