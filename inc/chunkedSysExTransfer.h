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
//  reliability fixes baked in here (post-chunk delay, periodic ID-request
//  resend) and why they were needed.
//
//  SPDX-License-Identifier: MIT
//

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "RtMidi.h"
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

// Sends a Universal Non-Realtime Device Inquiry (F0 7E 7F 06 01 F7) and waits
// up to waitMs for any SysEx reply (delivered via chunkReplyCallback).
//
// A single request/reply round trip occasionally gets lost (either direction)
// without the device actually being unresponsive - observed in practice as a
// chunk aborting a transfer even though the target board answers fine
// immediately afterward. Re-send the request periodically within the waitMs
// budget instead of a single fire-and-wait, so one dropped packet doesn't
// abort an otherwise-healthy transfer.
inline bool waitForIdReply(RtMidiOut &midiOut, ChunkReplyState &state, unsigned int waitMs)
{
    static const std::vector<unsigned char> idRequest = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    state.received = false;

    const unsigned int pollIntervalMs = 5;
    const unsigned int resendIntervalMs = 300;
    unsigned int waited = 0;
    unsigned int sinceLastSend = 0;

    midiOut.sendMessage(&idRequest);
    while (!state.received && waited < waitMs)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
        waited += pollIntervalMs;
        sinceLastSend += pollIntervalMs;

        if (!state.received && sinceLastSend >= resendIntervalMs && waited < waitMs)
        {
            midiOut.sendMessage(&idRequest);
            sinceLastSend = 0;
        }
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
// already >= every chunk's length (the normal, expected case), each chunk
// goes out as a single, whole, correctly-framed message. IMPORTANT: chunkSize
// smaller than an actual chunk's on-wire length (payload + hex-record header)
// causes that message to be split with no F0/F7 framing on the pieces, which
// is not a valid SysEx split and can corrupt that message - callers should
// pass a chunkSize comfortably at or above the packaging tool's chunk size,
// not try to cut it close (see k-board-firmware's commands.md, 2026-07-16).
inline bool sendChunkedFileToPort(RtMidiOut &midiOut,
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
                std::cout << "(sub-split into " << chunkSize << "-byte windows, chunk size < message length) ";
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
            // Give the device a moment to finish receiving/start processing the
            // chunk before demanding a reply - firing the ID request immediately
            // risks it landing while the chunk's tail end is still being handled
            // on the device side and getting missed.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

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

#endif // CHUNKED_SYSEX_TRANSFER_H
