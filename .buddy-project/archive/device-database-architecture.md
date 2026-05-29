# Device Database and Update Architecture

## Purpose

The project needs a clear separation between three responsibilities:

1. SendSysEx as a small command-line host and raw sender.
2. kmiDevice as the device-oriented firmware update controller.
3. deviceDatabase as the single source of truth for product metadata, payload files, and normalized port matching.

This structure keeps the code easy to follow and avoids repeated JSON parsing, fuzzy name checks, and update-specific logic spread across multiple files.

---

## Why the device database exists

The device database should own all metadata needed to identify and update supported KMI products.

That includes:

- family identifiers such as SoftStep, QuNeo, QuNexus, 12 Step, K-Board, and KBP4
- payload discovery and filename resolution
- identity and PID expectations
- known application and bootloader port layouts
- backend-specific raw port naming quirks
- exact normalized port names used for matching

The goal is to stop guessing from partial strings and instead make decisions from a normalized, canonical representation.

---

## Role of the family JSON files

Each family JSON file should describe one product family and should remain the canonical human-editable metadata source.

A family file should answer the following questions:

- What product family is this?
- What bootloader and application states exist?
- Which payloads are available for each state?
- Which raw or normalized port names belong to this family?
- Which names are safe to probe for identity requests?
- Which names indicate bootloader state?

### Family JSON responsibilities

Recommended responsibilities for each family file:

- discovery markers and exact normalized port names
- state-specific port layouts for input and output
- device identity expectations such as manufacturer and product IDs
- firmware payloads and bootloader-entry payloads
- notes about legacy behavior and OS-specific edge cases

### Important rule

The JSON should describe the family in a structured way.

The code should not hardcode ad hoc device-specific name matching logic. Instead, the code should ask the device database for the exact normalized names and expected states.

---

## Why fuzzy matching is a problem

Using loose matching such as looks like, contains, or substring heuristics is brittle.

Examples of failure modes:

- a Windows alias like MIDI2 (SoftStep) matches when it should not
- a bootloader port is mistaken for an application port
- a service or share port is selected instead of the main control port
- different backends expose the same device with different naming formats

This causes false positives and makes update behavior unpredictable.

### Design rule going forward

After normalization, port matching should be exact.

That means:

- normalize the raw RtMidi name according to the active OS and backend
- reduce it to a canonical form
- compare against known exact names from the device database
- determine the role and state from the normalized entry, not from fuzzy search logic

---

## Port-name normalization strategy

RtMidi exposes different raw names depending on platform and backend. The device database layer should absorb that variation and produce a canonical name model.

### Normalization goals

The normalization pipeline should:

- trim whitespace
- collapse repeated spaces
- lowercase for canonical comparison
- remove backend wrappers where appropriate
- strip Windows numbering prefixes when they are aliases rather than actual device descriptors
- preserve meaningful state indicators such as bootloader
- map backend-specific raw names to a stable canonical name

### Backend-aware normalization

Normalization should take the active backend into account. 

#### Windows WinMM and WinUWP

Both backends will list the first device port as the device name, so "SoftStep" would be the "SoftStep Control Surface" port.

For devices with multiple ports, WinMM will report:

- MIDIIN2 (SoftStep) 3
- MIDIOUT2 (SoftStep Bootloader) 4

And WinUWP will report:

- MIDIIN2 (SoftStep)
- MIDIOUT2 (SoftStep Bootloader)

Which translates to:
MIDI[in/out][device port number] ([device name]) [os port number]

#### macOS CoreMIDI

CoreMIDI will display devices with named device and port strings as:

QuNexus Control Surface

Where "Control Surface" is the usb descriptor string of the first port. 

Devices with multiple ports that don't have usb descriptor strings will be reported as:

QuNexus Port 1

This can also be complicated as non-english versions of MacOS may translate the word "Port", so an espanol system will report:

QuNexus Peurto 1

#### Linux ALSA or JACK if added later

Linux backends reports unnamed port names like this:
SoftStep Bootloader:SoftStep Bootloader MIDI 1 16:0
SoftStep Bootloader:SoftStep Bootloader MIDI 2 16:1

Which translates to:

[device name]:[device name] MIDI [device port number] [alsa client number]:[alsa port number]

Linux backends report named port names in this format:

SoftStep:SoftStep Control Surface 16:0
SoftStep:SoftStep Expander 16:1

Which translates to:

[device name]:[device name] [port name] [alsa client number]:[alsa port number] 

### Key principle

Using the above formats and the device family json we should be able to normalize rtmidi ports to exact matches. 

---

## Proposed deviceDatabase class

The deviceDatabase class should become the only class responsible for parsing JSON, resolving payloads, and normalizing device names.

### deviceDatabase responsibilities

- load and validate family JSON files
- expose family definitions through a clean API
- resolve payload paths by type and version
- normalize raw RtMidi names by backend and OS
- map normalized names to family, role, and state
- provide exact lists of allowed application and bootloader ports
- provide safe probe-port selections
- avoid repeated JSON parsing inside kmiDevice

### Example responsibilities that should move into deviceDatabase

- name normalization logic
- role inference from normalized names
- choosing the correct comms ports for application and bootloader states
- payload filename lookup
- family marker and bootloader marker lookup

---

## Proposed role of kmiDevice

kmiDevice should be the active device controller, not the metadata parser.

### kmiDevice responsibilities

- watch for connection and disconnection events by polling RtMidi port lists
- use deviceDatabase to normalize and classify the visible ports
- own its own scan handles and comms handles
- send identity requests through MIDI_CPP
- parse identity replies and determine actual state
- enter bootloader when needed
- send firmware payloads using the resolved files from deviceDatabase
- track update state and final success or failure

### Internal handle separation

To avoid Windows port-access conflicts, kmiDevice should maintain separate handles for:

- scanning current visible ports
- application or bootloader communications including firmware transfer

This separation is especially important on Windows, where port reopening and shared access can behave differently across backends.

---

## Proposed role of SendSysEx

SendSysEx should remain a simple host application.

### Mode 1: raw sender mode

In raw sender mode, SendSysEx should:

- list the available ports exactly as RtMidi reports them
- allow the user to choose a raw port number or raw port name
- open the selected output directly with RtMidi
- send the selected SysEx file

This mode is intentionally low-level and transparent.

### Mode 2: automatic update host mode

In automatic mode, SendSysEx should:

- parse the family and version arguments
- create a kmiDevice instance
- release any local RtMidi objects before handing control over
- wait until kmiDevice returns success or failure
- print status and exit cleanly

In this mode, SendSysEx should not contain the update state machine itself.

---

## File and class layout

The codebase should be organized so that each class has one obvious purpose.

### Suggested layout

## Current implementation snapshot

The refactor now maps directly to these files:

- SendSysEx.cpp is the thin CLI host for raw sends and automated handoff.
- kmiDevice.h and kmiDevice.cpp own the device-oriented update controller.
- deviceDatabase.h and deviceDatabase.cpp own family JSON loading, payload resolution, and exact normalized port matching.
- deviceHelpers.h holds the small shared parsing and normalization helpers used across the project.

The intent is that SendSysEx stays small, kmiDevice stays focused on device state and update control, and deviceDatabase stays the single source of truth for metadata and port classification.

#### SendSysEx

- header or local helpers for CLI parsing and raw-send utilities
- main program entry point
- no product-specific update logic
- no JSON parsing
- no port normalization rules

#### kmiDevice

- device lifecycle and update state machine
- MIDI_CPP identity request and reply handling
- polling, transitions, timeout handling, and firmware update orchestration
- depends on deviceDatabase for all metadata queries

#### deviceDatabase

- load and cache family JSON data
- exact normalized name matching
- payload lookup
- family and state metadata queries

#### small helper headers

Short utility functions that do not need to live in a large implementation file should be moved to focused headers, for example:

- string normalization helpers
- file-loading helpers
- version parsing and formatting helpers
- progress formatting helpers

This keeps implementation files smaller and easier to review.

---

## Code-style and modularization guidance

To keep the project clean and readable:

- keep classes focused on one responsibility
- avoid large monolithic source files
- move short reusable helpers to headers when appropriate
- avoid duplicating parsing logic in multiple classes
- prefer straightforward control flow with small helper functions
- for single-line conditionals, omit braces and place the one statement on the next line when that matches project style
- keep public APIs minimal and expose only what the host actually needs

### Practical modularization target

A reader should be able to understand the project by reading in this order:

1. SendSysEx host flow
2. kmiDevice update controller
3. deviceDatabase metadata source
4. family JSON definitions

That gives a clean top-down understanding of the system.

---

## Recommended next refactor steps

1. Add a deviceDatabase class in its own header and source files.
2. Move JSON parsing and payload resolution out of kmiDevice into deviceDatabase.
3. Move port normalization and exact name classification into deviceDatabase.
4. Refactor kmiDevice to depend on deviceDatabase rather than reading JSON directly.
5. Remove the manual bootloader update state machine from SendSysEx.
6. Keep SendSysEx only as a raw sender and automatic-update host.
7. Verify both WinMM and UWP behavior with the same normalized database-driven matching rules.

---

## Summary

The device database and family JSON files should define what the devices are.

kmiDevice should define how the update process runs.

SendSysEx should define how the user launches either a raw send or an automatic update.

The current implementation now follows that split, with exact normalized name matching handled in deviceDatabase rather than scattered fuzzy checks in the sender or device controller.

That separation reduces brittleness, avoids fuzzy port matching bugs, and makes the project much easier to maintain.