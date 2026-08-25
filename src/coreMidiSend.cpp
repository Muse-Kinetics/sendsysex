//
//  coreMidiSend.cpp  -  see coreMidiSend.h
//
//  SPDX-License-Identifier: MIT
//

#include "coreMidiSend.h"

#ifdef __APPLE__

#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <iostream>

namespace
{

std::string cfToStd(CFStringRef s)
{
    if (!s)
        return std::string();
    char buf[1024];
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8))
        return std::string(buf);
    return std::string();
}

std::string endpointName(MIDIEndpointRef ep)
{
    CFStringRef name = 0;
    if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &name) == noErr && name)
    {
        std::string s = cfToStd(name);
        CFRelease(name);
        return s;
    }
    return std::string();
}

// lowercase + collapse runs of whitespace to single spaces + trim
std::string normalize(const std::string &in)
{
    std::string out;
    bool prevSpace = false;
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (std::isspace(c))
        {
            if (!prevSpace && !out.empty())
                out.push_back(' ');
            prevSpace = true;
        }
        else
        {
            out.push_back(static_cast<char>(std::tolower(c)));
            prevSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

struct SendCtx
{
    std::atomic<bool> done{false};
};

// Called on CoreMIDI's sysex-sending thread when the transfer finishes.
void completionProc(MIDISysexSendRequest *request)
{
    SendCtx *ctx = static_cast<SendCtx *>(request->completionRefCon);
    if (ctx)
        ctx->done.store(true);
}

void noopCompletion(MIDISysexSendRequest *) {}

// CoreMIDI caches, per client, the (last destination, its max sysex speed) it
// was given for MIDISendSysex - so a freshly-set speed on our destination is
// ignored until we send a tiny sysex to a DIFFERENT endpoint to bust that
// cache (SysEx Librarian's forceCoreMIDIToUseNewSysExSpeed). No-op if there's
// no other destination to poke.
void bustSysExSpeedCache(MIDIEndpointRef ours)
{
    const ItemCount n = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; ++i)
    {
        MIDIEndpointRef ep = MIDIGetDestination(i);
        if (ep && ep != ours)
        {
            static const Byte tiny[2] = {0xF0, 0xF7};
            MIDISysexSendRequest breq;
            breq.destination = ep;
            breq.data = tiny;
            breq.bytesToSend = 2;
            breq.complete = false;
            breq.reserved[0] = breq.reserved[1] = breq.reserved[2] = 0;
            breq.completionProc = noopCompletion;
            breq.completionRefCon = 0;
            MIDISendSysex(&breq);
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, false);
            return;
        }
    }
}

} // namespace

bool sendSysExViaCoreMIDI(const std::string &targetName,
                          const std::vector<unsigned char> &bytes,
                          int maxSpeedBytesPerSec,
                          std::string &err)
{
    if (bytes.empty() || bytes.front() != 0xF0 || bytes.back() != 0xF7)
    {
        err = "CoreMIDI send expects a complete F0..F7 SysEx buffer.";
        return false;
    }

    // Resolve the destination endpoint by display name.
    const ItemCount numDest = MIDIGetNumberOfDestinations();
    const std::string tnorm = normalize(targetName);
    MIDIEndpointRef dest = 0;
    std::string destName;
    std::string available;
    for (ItemCount i = 0; i < numDest; ++i)
    {
        MIDIEndpointRef ep = MIDIGetDestination(i);
        const std::string nm = endpointName(ep);
        available += "[" + nm + "] ";
        const std::string nn = normalize(nm);
        // Skip nameless endpoints - normalize("") == "" and find("") == 0 would
        // otherwise make an unreadable endpoint match ANY target.
        bool match = false;
        if (!nn.empty() && !tnorm.empty())
        {
            match = (nn == tnorm) ||
                    (tnorm.size() >= 3 && nn.find(tnorm) != std::string::npos) || // target is a substring of the port
                    (nn.size() >= 3 && tnorm.find(nn) != std::string::npos);      // port is a substring of the target
        }
        if (match && !dest)
        {
            dest = ep;
            destName = nm;
        }
    }
    if (!dest)
    {
        err = "No CoreMIDI destination matched \"" + targetName + "\". Available destinations: " +
              (available.empty() ? "(none)" : available);
        return false;
    }

    MIDIClientRef client = 0;
    OSStatus st = MIDIClientCreate(CFSTR("SendSysEx-CoreMIDI"), 0, 0, &client);
    if (st != noErr)
    {
        err = "MIDIClientCreate failed (OSStatus " + std::to_string((int)st) + ").";
        return false;
    }

    // Report the endpoint's advertised max sysex speed, and optionally clamp it.
    // MIDISendSysex paces to this property; a USB-MIDI driver advertising a high
    // rate makes it blast a device that can only keep up at ~1x MIDI speed.
    {
        SInt32 curSpeed = 0;
        if (MIDIObjectGetIntegerProperty(dest, kMIDIPropertyMaxSysExSpeed, &curSpeed) == noErr)
            std::cout << "  endpoint advertises maxSysExSpeed = " << curSpeed << " bytes/sec\n";
        else
            std::cout << "  endpoint has no maxSysExSpeed property (CoreMIDI default ~3125)\n";
    }
    if (maxSpeedBytesPerSec > 0)
    {
        MIDIObjectSetIntegerProperty(dest, kMIDIPropertyMaxSysExSpeed,
                                     static_cast<SInt32>(maxSpeedBytesPerSec));
        bustSysExSpeedCache(dest);
        std::cout << "  forced maxSysExSpeed = " << maxSpeedBytesPerSec
                  << " bytes/sec (cache busted)\n";
    }

    SendCtx ctx;

    MIDISysexSendRequest req;
    req.destination = dest;
    req.data = bytes.data();
    req.bytesToSend = static_cast<UInt32>(bytes.size());
    req.complete = false;
    req.reserved[0] = req.reserved[1] = req.reserved[2] = 0;
    req.completionProc = completionProc;
    req.completionRefCon = &ctx;

    std::cout << "CoreMIDI MIDISendSysex -> \"" << destName << "\"  ("
              << bytes.size() << " bytes, flow-controlled async)\n";

    st = MIDISendSysex(&req);
    if (st != noErr)
    {
        err = "MIDISendSysex failed (OSStatus " + std::to_string((int)st) + ").";
        MIDIClientDispose(client);
        return false;
    }

    // CRITICAL: MIDISendSysex is asynchronous and its pacing is driven off the
    // run loop of the thread that created the MIDIClient (here, the main
    // thread). We must SPIN that run loop while waiting - NOT block it on a
    // condition variable. Blocking it starves the pacing machinery, so CoreMIDI
    // dumps the whole buffer to USB in one unspaced burst, overrunning a device
    // doing blocking flash erases and bricking it (observed on real v93
    // SoftStep hardware). SysEx Librarian works precisely because it's a GUI
    // app with a live run loop. So: run the loop in short slices, checking the
    // completion flag (set on CoreMIDI's thread) between slices.
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(300);
    bool timedOut = false;
    while (!ctx.done.load())
    {
        // Run the loop for a full slice (returnAfterSourceHandled = false) so
        // CoreMIDI's pacing timers fire uninterrupted.
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2 /* seconds */, false);

        // req.bytesToSend is decremented by CoreMIDI as it sends.
        const UInt32 remaining = req.bytesToSend;
        const std::size_t sent = bytes.size() - remaining;
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("\r  sent %zu / %zu bytes (%.0f%%)  %.1fs  %.0f B/s   ", sent, bytes.size(),
                    bytes.empty() ? 100.0 : (100.0 * sent / bytes.size()), elapsed,
                    elapsed > 0.01 ? sent / elapsed : 0.0);
        std::fflush(stdout);

        if (std::chrono::steady_clock::now() > deadline)
        {
            timedOut = true;
            break;
        }
    }
    std::printf("\n");

    MIDIClientDispose(client);

    if (timedOut && !ctx.done.load())
    {
        err = "MIDISendSysex did not complete within timeout (device unresponsive?).";
        return false;
    }
    return true;
}

#else // !__APPLE__

bool sendSysExViaCoreMIDI(const std::string &, const std::vector<unsigned char> &, std::string &err)
{
    err = "CoreMIDI send (--coremidi) is only available on macOS.";
    return false;
}

#endif
