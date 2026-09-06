# Current Task

## Status: v0.15.0 shipped and solid on macOS + Windows. Linux/ALSA `--bootloader-install` root-caused and FIXED — validated end-to-end on hardware.

Branch `main`. Version **0.15.0** (`CMakeLists.txt`, README in sync).

### Where the release line stands (DONE)

- **Windows validation complete.** The chunked `--fw-update` reliability work was re-run on real
  hardware over both **WMS and WinMM** — no regressions. This was the release gate; it is cleared.
- **v0.15.0 is a solid release for both macOS and Windows** — chunked `--fw-update` across the whole
  family, interactive mode, and the universal notarized macOS `.pkg` installer.
- **Bootloader upgrade (`--bootloader-install`) is tested and working** on macOS and Windows for both
  SoftStep and 12 Step.

### Active investigation (2026-09-06) — `--bootloader-install softstep` fails on Linux/ALSA

**ROOT-CAUSED. Not an ALSA SysEx gating problem.** Two independent faults, one environmental and one
a real code bug that is a **safety issue on every platform**.

#### Fault 1 (environment): incomplete alsa-lib 1.2.14 install — FIXED

The ALSA update installed a source build of **alsa-lib 1.2.14** at `/usr/local/lib/libasound.so.2`
(wanted for **MIDI 2.0 / UMP** support), which `ldconfig` resolves ahead of the distro library. It was
configured with no `--prefix`, so it baked in `ALSA_PLUGIN_DIR = /usr/local/lib/alsa-lib` — but
`alsa-plugins` was never built, so that directory did not exist. Every ALSA client then failed at
open:

```
ALSA lib conf.c:4029: Cannot open shared library libasound_module_conf_pulse.so
    (/usr/local/lib/alsa-lib/libasound_module_conf_pulse.so: No such file or directory)
ALSA lib seq.c:1011:(snd_seq_open_noupdate) Unknown SEQ default
MidiOutAlsa::initialize: error creating ALSA sequencer client object.
```

The trigger is the distro `/etc/alsa/conf.d/99-pulse.conf`, which 1.2.14's own `alsa.conf` still
loads: it requests `libasound_module_conf_pulse.so` by bare name, 1.2.14 looks only in its own empty
plugin dir, and config parsing aborts before the sequencer is ever reachable. So it broke *every*
ALSA client — `aconnect -l` included — not just SendSysEx.

**Fix: `scripts/fix-alsa-plugin-dir.sh`** (`--check` / `--undo` modes included). It symlinks
`/usr/local/lib/alsa-lib` → `/usr/lib/x86_64-linux-gnu/alsa-lib`, so the existing `libasound2-plugins`
package satisfies the lookup. 1.2.14 stays in place, along with its MIDI 2.0 / UMP tools
(`aplaymidi2`, `arecordmidi2`, `aseqsend`). Compatibility verified first: both libraries carry SONAME
`libasound.so.2`, 1.2.14 exports the `ALSA_0.9` version node the plugins are linked against, and
`conf_pulse` imports exactly one libasound symbol (`snd_config_hook_load`), stable across versions.

Rejected alternatives: the `ALSA_PLUGIN_DIR` env var (only helps processes that set it — same
shortcoming as `LD_LIBRARY_PATH`); rebuilding `alsa-plugins` from source (pulls in
PulseAudio/JACK/speex/samplerate dev deps to duplicate working packages); deleting the pulse config
(breaks PulseAudio routing for every other app).

**Verified after the fix:** `aconnect -l` opens cleanly, and SendSysEx runs natively against 1.2.14
with **no `LD_LIBRARY_PATH`** — ports enumerate and `--id-request softstep` returns bootloader 1.0.0 /
application 2.0.7.

Note this fault post-dates the original failing run: that run *did* enumerate ports and reach the
device, so Fault 2 is what actually broke it.

#### Fault 2 (code bug): `captureFirmwareDump` hears its own request echoed back

The captured "firmware image" is **byte-for-byte identical to `kSoftStepDumpRequest`** — verified,
all 75 bytes. The tool did not receive a truncated dump; it received **its own outgoing request**.

Mechanism: `captureFirmwareDump` (`src/SendSysEx.cpp:1656`) opens **every** MIDI input port on the
system and accepts the first message that starts with `F0` and is longer than 4 bytes, from any of
them. On Linux, ALSA's **`Midi Through`** port (client 14) loops output straight back to input, so
the request lands in the callback ~instantly and satisfies the wait. The same shape would occur with
any loopback/virtual port, or a physical MIDI DIN out-to-in cable, on macOS or Windows.

Validation then **false-passed** it: the check at `src/SendSysEx.cpp:1873` is
`size > 10 && front==0xF0 && back==0xF7 && header matches`. The request naturally satisfies all four,
because it carries the same KMI header. So the flow printed "Image validated", proceeded to flash
the trojan onto a device whose real firmware was never read, and then unsurprisingly got no reply to
any of the 10 post-install identity requests.

The `reply - version ?` lines are consistent with the same echo path in `runLegacyVersionQuery`.

#### Fixes — IMPLEMENTED and hardware-verified (2026-09-06)

All three landed in `src/SendSysEx.cpp` (uncommitted; awaiting approval):

1. **Listen only on the device's own input ports.** New `openDeviceInputPorts()` /
   `closeDeviceInputPorts()` helpers open only inputs whose port name carries the family marker
   (`SSCOM`, `12`), so `Midi Through` and other loopbacks are never opened. Both
   `captureFirmwareDump` and `runLegacyVersionQuery` now use them (they previously each opened every
   input port on the system).
2. **Reject an echo of the request.** `DumpState` and `LegacyReplyState` now carry the request bytes;
   a captured message equal to what was just sent is ignored rather than accepted as a reply.
3. **Minimum plausible image size.** Dump validation now requires **≥ 4096 bytes** in addition to the
   header check. Every shipped SoftStep/12 Step image is 76–110 KB, so the bound is conservative. The
   error message names the likely cause (a MIDI loopback) so the next person is not left guessing.

**Hardware verification on the real SoftStep (Linux/ALSA):**

- `--id-request softstep` now reports **version 93** — previously `version ?` on 5/5 attempts. The
  echo had been masking the device's genuine reply. v93 matches the unit recorded in `decisions.md`.
- `--bootloader-install softstep` dump gate captured **66,312 bytes** (a real image) where the old
  code captured the 75-byte echo. Connection validation read 5/5 `version 93`.

All three are platform-independent hardening. (1) and (2) also explain why the path worked on
macOS/Windows: no `Midi Through` equivalent is present by default.

#### Fault 3 (TX): ALSA rejected the legacy sector send — FIXED

With RX fixed, the send got further and hit `MidiOutAlsa::sendMessage: incomplete message!` on every
span. **This was the original concern, and it was correct.**

*How ALSA handles SysEx:* the legacy trojan image is **one single SysEx message** (the 80,580-byte
SoftStep payload has exactly one `F0` and one `F7`), deliberately split into ~100 separately-timed
spans so the block-to-data / sector / bank gaps land as real wire gaps. Every span but the first is
therefore not a self-contained message. `MidiOutAlsa::sendMessage()` re-encodes through
`snd_midi_event_encode()`, which yields `SND_SEQ_EVENT_NONE` for a non-self-contained span, so RtMidi
drops it (`inc/rtmidi/RtMidi.cpp:2806`). Verified directly against alsa-lib: opening `F0…` (no `F7`),
bare middle spans, and a trailing `…F7` all encode to `EVENT_NONE`; only a whole `F0…F7` produces an
event. The encoder *does* accumulate across calls and emit at the closing `F7` — but that collapses
the entire image into one event and destroys the very inter-span timing the send exists to control,
so it is not a usable path.

*The alternate send method (answering the question directly):* **yes** — `snd_seq_ev_set_sysex()`
bypasses the encoder and takes an arbitrary byte span verbatim. That is ALSA's own native transport
for large SysEx (segmented spans, concatenated by the receiver until `F7`) — the mirror image of the
reassembly RtMidi's ALSA *input* side already does.

*Fix:* handled **inside `MidiOutAlsa::sendMessage()`** — **no public API change**, `RtMidi.h` is
untouched. `AlsaMidiData` gains a `sysexInProgress` flag (mirroring the one the fork's `MidiOutCore`
already carries); when a call starts with `F0` or the port is mid-SysEx, the bytes go out via
`snd_seq_ev_set_sysex()`, bypassing the encoder. Everything else takes the original path. The caller
side is unchanged — `sendBytes()` and the bank-transition probe call plain `sendMessage()` as before.
**The timing/chunking model is untouched.**

An earlier draft added a public `sendMessageFragment()` virtual; it was reworked because that shape
is not defensible upstream (new vtable entry = ABI break; two ways to do one thing whose difference
is invisible on 8 of 9 backends; leaks an ALSA detail into the cross-platform surface). As an
internal fix it is instead a plain cross-platform consistency bug — CoreMIDI/WinMM/JACK/Web all
accept a partial SysEx run; ALSA silently dropped it. See `decisions.md`.

#### End-to-end result — first successful bootloader install on Linux (2026-09-06)

```
SoftStep legacy v93 (no bootloader)
  -> --bl-send trojan: 100/100 sectors, ZERO "incomplete message!" errors
  -> VERIFY OK: device in bootloader mode, bootloader v1.0.0 (1f38:000e)
  -> --fw-update softstep: 290/290 chunks, every one ACKed (110,532 bytes)
  -> Confirmed application version: 2.0.7  (1f38:000c, state=application)
```

### Priority Order (Next)

1. **Review + commit** the three RX fixes and the ALSA TX fix — all implemented and hardware-verified
   end-to-end, awaiting approval. The RX fixes are safety-relevant on all platforms; the TX fix is
   confined to `MidiOutAlsa::sendMessage()` in the KMI RtMidi fork (`RtMidi.cpp` only — `RtMidi.h` is
   untouched).
2. **Re-validate macOS + Windows** for the trojan install path. The ALSA change cannot affect them
   (it is inside `#if defined(__LINUX_ALSA__)`), but a confirming run before release is cheap.
3. **Consider an upstream RtMidi PR** for the ALSA fix — it is a self-contained backend bug fix with
   no API surface: ALSA silently drops partial SysEx that every other backend accepts. See
   `decisions.md` for the framing.
4. **Decide whether Linux joins the supported platform list** — the full path now works; it is a
   packaging/support-policy call, not a technical blocker.
5. **Reconcile the `WMS` branch** (`origin/WMS`, `56580a7`): it carries a cmake/rtmidi refactor
   ("use rtmidi add_subdirectory; drop redundant WMS wiring") not in the release line. Decide whether
   it merges. See handoff.md "Branch landscape".

### Phase

Post-v0.15.0 · Linux/ALSA bootloader-install fixed end-to-end; changes uncommitted, pending review + cross-platform re-validation.
