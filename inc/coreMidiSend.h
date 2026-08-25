#ifndef CORE_MIDI_SEND_H
#define CORE_MIDI_SEND_H

//
//  coreMidiSend.h
//
//  PROTOTYPE: flow-controlled SysEx send on macOS via CoreMIDI's MIDISendSysex(),
//  bypassing RtMidi's MidiOutCore::sendMessage entirely.
//
//  Why: RtMidi switched its CoreMIDI output from MIDISendSysex() to
//  MIDIPacketListAdd()/MIDISend() in upstream commit 94a04ef (2014-07-20,
//  "rather than using MIDISendSysex, use MIDIPacketListAdd() and MIDISend()"),
//  to dodge a >256-byte bug. That threw away the asynchronous, flow-controlled
//  delivery MIDISendSysex provides - the driver paces the transfer and honors
//  USB back-pressure, so a device doing blocking flash erases (legacy v93
//  SoftStep / 12 Step firmware) isn't overrun and corrupted. A plain MIDISend
//  blast (even paced with host-side sleeps) still bursts each buffer at full
//  USB speed and bricks the device; MIDISendSysex does not. SysEx Librarian
//  (snoize/MIDIApps) still uses MIDISendSysex, which is why it works.
//
//  This is the reference implementation for the eventual RtMidi CoreMIDI-backend
//  fix (restore MIDISendSysex for the sysex path); kept application-side first
//  so we can validate it on real v93 hardware before baking it into the library.
//
//  SPDX-License-Identifier: MIT
//

#include <string>
#include <vector>

// Sends the entire byte buffer (a complete F0..F7 SysEx, e.g. a legacy padded
// firmware image) to the CoreMIDI destination whose display name matches
// targetName (exact, else case-insensitive substring either way), using
// MIDISendSysex() and blocking until the asynchronous send completes. Prints
// progress. Returns false and fills err on any failure (no match, API error,
// timeout). On non-Apple platforms this is a stub that returns false.
// maxSpeedBytesPerSec: if > 0, set kMIDIPropertyMaxSysExSpeed on the
// destination to this value before sending (with the cache-bust workaround),
// so MIDISendSysex paces at a known rate instead of whatever the endpoint's
// driver advertises. 3125 = 1x MIDI DIN speed (SysEx Librarian's "100%").
// 0 = leave the endpoint's advertised speed untouched.
bool sendSysExViaCoreMIDI(const std::string &targetName,
                          const std::vector<unsigned char> &bytes,
                          int maxSpeedBytesPerSec,
                          std::string &err);

#endif // CORE_MIDI_SEND_H
