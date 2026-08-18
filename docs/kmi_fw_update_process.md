# KMI Firmware Update Process (C8051F380 Bootloader)

KMI products are either SiLabs C8051F380 based or STM32 based. This document describes the C8051F380 bootloader behavior.

## Flash Layout and Boot Flow

- Flash is split into a bootloader region (typically first `0x2400` bytes) and an application region (`0x2400` through the application end of flash).
- On power-up, the bootloader checks a boot byte to decide whether to run the application or stay in bootloader mode.
- If it tries to run the application, it marks the boot attempt first. The application must clear/update the boot byte after a successful boot.
- If the application fails to boot correctly, the next power cycle may remain in bootloader mode waiting for firmware update traffic.
- Depending on product-specific behavior, a full power cycle may be needed to reliably return to bootloader mode.

## USB and Update Transport Behavior

- The C8051 bootloader polls USB receive data continuously (no interrupt-driven receive path during update handling).
- It responds to SysEx identity requests.
- At boot, internal flag `flashErased` starts at `0`.
- On first valid firmware packet while `flashErased == 0`, the bootloader erases the full application area, then sets `flashErased = 1`.
- User-data regions reserved near the top of flash are preserved according to each product's flash map.

Historically, KMI updates are sent as one long KMI-formatted SysEx payload (often large on disk in `.syx` form, with decoded firmware data under 64 KB for C8051 targets). The transfer relies on USB flow control (`NAK`/`ACK`) rather than per-packet application-layer responses.

The bootloader performs blocking flash writes and depends on host-side USB flow-control compliance. If the host does not correctly honor `NAK`/`ACK`, packets can be dropped and firmware may be corrupted.

## Chunked Update Notes

Chunking has been explored as a reliability strategy:

- Send complete SysEx chunks in proper order.
- Optionally send SysEx identity requests between chunks and wait for reply before continuing.
- Do not split firmware command units across chunks.
- Do not cross flash page boundaries (512 bytes) within a chunk
- Keep flash payload ordering sequential (low address to high address).

Review each product bootloader implementation before using chunked transfer in production.

Use the hextosysex utility to create properly chunked .syx files from intel hex.

### `sendsysex`'s implementation of the above (2026-08 hardware findings)

`inc/chunkedSysExTransfer.h`'s `sendChunkedFileToPort()` is the concrete implementation of "send identity requests between chunks and wait for reply" for both the raw manual path (`-p`/`-n -f`) and `--fw-update`. Two products confirmed hardware requirements beyond the basic between-chunk handshake:

- **The first chunk needs its own small-window/long-settle treatment** (`-fcs`/`--first-chunk-size`, `-fgd`/`--first-gap-delay`): this doc's "on first valid firmware packet while `flashErased == 0`, the bootloader erases the full application area" is exactly why - the receiver recognizes a valid header within the first ~20-30 bytes and immediately starts a blocking flash-erase before it can accept more USB data or answer anything. A small first window clears the host fast enough to avoid landing mid-erase; the following gap needs to be several seconds, not the steady-state inter-chunk delay. First found on KBP4 (2026-08-13/15, `data/families/kbp4.json`'s `transport.firmwareUpdateDefaults`), independently confirmed on K-Board (2026-08-18, `data/families/kboard.json`'s own `transport.firmwareUpdateDefaults`) - likely applies to any C8051 product doing the same unconditional erase-on-first-packet.
- **The post-chunk identity-reply wait must match whichever grace period that chunk's *send* used, not always the steady-state `-cd`/`--chunk-delay`** (fixed 2026-08-18): the reply wait after chunk 1 now uses `max(chunkDelayMs, firstGapDelayMs)`, and after the final chunk `max(chunkDelayMs, postDelayMs)` - both chunks were failing "NO REPLY" on real hardware even with `-fgd`/`-pd` explicitly passed, because the verification step ignored them and used the short `-cd` value regardless. Confirmed fixed against a real K-Board update (`-fcs 64 -fgd 3000 -cs 512 -cd 150 -pd 3000` now completes with no errors).
- **The final chunk's reply is now actually checked** (fixed 2026-08-18, was previously skipped entirely - see `chunkedSysExTransfer.h`'s inline comments): a transport-level "sent successfully" no longer means the transfer landed. A real update reported success this way while the EOF chunk had silently been dropped, leaving the device stuck in bootloader with no error surfaced.

## EOF and Reboot Behavior

- A normal update ends with an `END_OF_FILE` Intel HEX record.
- After receiving EOF, the bootloader reboots and attempts to launch the application.
- If an update fails before EOF, the bootloader typically remains active and can still answer identity requests.

In this failed-update state, a host can force an erase+reboot command. Note that rebooting after erase may attempt to boot erased application space, which can crash and then fall back to bootloader on the next power cycle.

## Erase+Reboot Command (Double EOF)

The command below is a KMI SysEx frame containing two EOF records. The second EOF causes erase-before-reboot behavior in the deployed C8051 bootloader flow.

```text
0xF0 0x00 0x01 0x5F 0x7A [PID]
0x00 0x00 0x00 0x00 0x00 0x01
0x00 0x02 0x11 0x10 0x48 0x53 0x00 0x30
0x03 0x07 0x3A 0x00 0x00 0x00 0x01 0x00 0x7F 0x64 0x40 0x00 0x00 0x00 0x00 0x01
0x03 0x07 0x3A 0x00 0x00 0x00 0x01 0x00 0x7F 0x64 0x40 0x00 0x00 0x00 0x00 0x01
0xF7
```

Byte map:

- Bytes 1-6: KMI SysEx header (manufacturer IDs, `[PID]`, format `0`)
- Bytes 7-11: Four zero padding bytes plus packet-start marker `0x01`
- Bytes 12-19: Encoded preamble (`START_OF_TEXT = 0x0002`, packet/category id `0x1110` for firmware packet, preamble CRC `0x4853`) plus alignment bytes
- Bytes 20-35: First encoded EOF Intel HEX record (`SX_HEX_LINE_START`, total length `7`, colon `0x3A`, length `0`, address `0x0000`, type `1` for EOF, hex CRC `0xFF`, line CRC `0x6440`)
- Bytes 36-51: Second encoded EOF record (identical)
- Final byte: SysEx end `0xF7`


