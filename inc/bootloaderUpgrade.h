#ifndef BOOTLOADER_UPGRADE_H
#define BOOTLOADER_UPGRADE_H

//
//  bootloaderUpgrade.h
//
//  Decode + sector-wise send support for LEGACY (pre-bootloader, v93-era)
//  SoftStep / 12 Step firmware images - the "bootloader trojan horse" update
//  path. These images are fundamentally different from the modern
//  Hex_to_SysEx chunked format that chunkedSysExTransfer.h handles:
//
//    * Modern chunked .syx  = many independent F0..F7 messages, each its own
//      CRC-framed chunk; the bootloader ACKs between them.
//    * Legacy trojan .syx   = ONE F0..F7 message containing many KMI packets:
//      per bank, a FW_HEADER followed by repeated [FW_BLOCK_HEADER, FW_DATA]
//      512-byte-sector pairs, then a second bank, then F7. Captured from a
//      device's own send_fw_update() (see the SoftStep firmware history,
//      commit 62e3d57 Softstep2/fwupdate.c / sysex.c).
//
//  The whole image MUST be re-sent as a single F0..F7: the receiver's
//  sx_process() resets its stage flag (second_stage = 0) on EVERY F0, so
//  splitting the banks/sectors into separate SysEx messages silently
//  corrupts the app-bank write. We therefore decode the one message into
//  ordered sectors here, and pace them out INSIDE one open message elsewhere
//  (interactive step or timed), giving per-sector and bank-transition timing
//  control the baked-in zero-padding can't guarantee under modern USB.
//
//  Decoding uses MIDI_CPP's own 7bit<->8bit routines and CRC (decode_get,
//  crc_buf) so it is byte-exact with what the firmware produced/expects,
//  never a re-implementation. Each packet's original wire bytes are retained
//  verbatim for replay, so a faithful send never re-encodes anything.
//
//  Two verbatim representations are kept per packet:
//    * ...Wire  = just the packet's own bytes (0x01 + encoded body + flush pad),
//                 with the file's inter-packet padding removed.
//    * ...Raw   = the packet PLUS the file's trailing inter-packet padding, cut
//                 so that concatenating outerHeaderRaw + every bank/sector Raw
//                 span in order reproduces the source file byte-for-byte.
//  The padding is the image's baked-in pacing (the receiving 8051 chews the zero
//  bytes as no-op data while it erases/writes flash). Sending the Raw spans
//  therefore reproduces the exact, hardware-validated byte stream; the Wire
//  spans are the padding-free experiment path where the sender supplies its own
//  wall-clock gaps instead.
//
//  SPDX-License-Identifier: MIT
//

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace blupgrade
{

// One 512-byte flash sector: a FW_BLOCK_HEADER packet (carries the block
// number that determines the receiver's write/erase address) immediately
// followed by a FW_DATA packet (the raw firmware bytes for that sector).
// The receiver erases the target sector when it processes the block header,
// then writes the data - which is why timing between the two, and between
// sectors, matters.
struct Sector
{
    uint8_t blockNum = 0;         // FW_BLOCK_HEADER.block_num -> write address
    uint16_t declaredLength = 0;  // FW_BLOCK_HEADER.length == FW_DATA payload len
    uint16_t dataLength = 0;      // actual decoded FW_DATA payload length

    // Padding-free wire bytes of each packet (0x01 + 7bit-encoded preamble+payload,
    // including the encoder's trailing flush padding to a group boundary), exactly
    // as they appear in the source file. Inter-packet zero padding is NOT included;
    // used only when the sender is asked to drop the padding and supply its own
    // wall-clock gaps instead.
    std::vector<uint8_t> blockHeaderWire;
    std::vector<uint8_t> dataWire;

    // Raw file spans that DO include the file's trailing inter-packet padding,
    // sliced so the whole image's Raw spans tile the file with no gap/overlap.
    // These reproduce the exact, hardware-validated byte stream. blockHeaderRaw
    // runs to the FW_DATA start; dataRaw runs to the next packet (or, for the
    // final sector, through the terminating F7).
    std::vector<uint8_t> blockHeaderRaw;
    std::vector<uint8_t> dataRaw;
    std::size_t blockHeaderRawBegin = 0; // byte offset of the 0x01 in the source file
    std::size_t dataRawBegin = 0;

    bool blockHeaderCrcOk = false;
    bool dataCrcOk = false;
};

// One firmware bank. bank 0 is the boot bank (staged to the top of flash by
// the receiver, then jumped into); bank 1 is the application bank (written at
// app_start). Both arrive in the same F0..F7 message.
struct Bank
{
    uint8_t bank = 0;
    uint16_t buildNumber = 0;
    uint16_t imageLength = 0;   // FW_HEADER.length (should equal sum of sector data)
    uint16_t imageCrc = 0;      // FW_HEADER.crc over the staged image
    uint8_t blockNumLast = 0;   // FW_HEADER.block_num_last
    std::string version;

    std::vector<uint8_t> headerWire;  // the FW_HEADER packet's padding-free wire bytes
    std::vector<uint8_t> headerRaw;   // FW_HEADER packet + trailing padding (raw span)
    std::size_t headerRawBegin = 0;   // byte offset of the 0x01 in the source file
    bool headerCrcOk = false;

    std::vector<Sector> sectors;
};

struct LegacyFirmwareImage
{
    // F0 + the 6-byte KMI standard header (mfg[3-4], product, format) exactly
    // as in the source file - sent once at the start of the single message.
    std::vector<uint8_t> outerHeaderWire;

    // Same prefix but extended through any leading padding up to the first
    // packet's 0x01 (raw-with-padding counterpart of outerHeaderWire). This is
    // the first tile of the byte-exact Raw reconstruction of the file.
    std::vector<uint8_t> outerHeaderRaw;

    std::vector<Bank> banks;

    bool ok = false;
    std::string error;

    std::size_t totalSectors() const;
    // True if every decoded packet's preamble + payload CRC verified.
    bool allCrcOk() const;
};

// Decode a legacy firmware .syx byte buffer into ordered banks/sectors.
// Verifies every packet's preamble and payload CRC with MIDI_CPP's routines.
// On structural failure sets ok=false and fills error; CRC failures are
// recorded per packet (blockHeaderCrcOk/dataCrcOk/headerCrcOk) but do not by
// themselves abort the decode, so a partially-corrupt file can still be
// inspected.
LegacyFirmwareImage decodeLegacyFirmwareSyx(const std::vector<uint8_t> &bytes);

// Print a human-readable manifest (banks, sectors, block numbers, lengths,
// CRC status) to stdout.
void printLegacyImageManifest(const LegacyFirmwareImage &img);

} // namespace blupgrade

#endif // BOOTLOADER_UPGRADE_H
