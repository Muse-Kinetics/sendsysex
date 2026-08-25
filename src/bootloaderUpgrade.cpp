//
//  bootloaderUpgrade.cpp
//
//  See bootloaderUpgrade.h for the why. Decodes a legacy single-message
//  firmware .syx into ordered banks/sectors using MIDI_CPP's own 7bit<->8bit
//  decode (SysExMessageRX::decode_put/decode_get) and CRC (crc_buf), so the
//  parse is byte-exact with the firmware that produced the image.
//
//  SPDX-License-Identifier: MIT
//

#include "bootloaderUpgrade.h"

#include <cstdio>
#include <iostream>

#include "MIDI_sysex.hpp"
#include "utils_crc.h"

namespace blupgrade
{

namespace
{

// Legacy KMI packet types (Softstep2/sysex.h enum). MIDI_CPP splits the old
// 2-byte "type" field into {category, type}; for these images category is
// always 0x00 and type is the low byte, so these map directly.
const uint8_t BLOCK_TYPE_FW_HEADER = 5;
const uint8_t BLOCK_TYPE_FW_BLOCK_HEADER = 6;
const uint8_t BLOCK_TYPE_FW_DATA = 7;

// Outer header on the wire is F0 + the 6-byte SYSEX_STANDARD (mfg[3], then two
// bytes that are product-MSB/product on modern devices but a 4th manufacturer
// byte + product on these legacy units, then format). Either way it is a fixed
// 6 bytes after F0 before packet data begins, and byte 5 (a product id of
// 0x01) must not be mistaken for an SX_PACKET_START.
const std::size_t kOuterHeaderLen = 1 + 6;

uint16_t be16(const std::vector<uint8_t> &b, std::size_t o)
{
    return static_cast<uint16_t>((b[o] << 8) | b[o + 1]);
}

// Modern (unsigned-char) KMI CRC, straight from MIDI_CPP's utils_crc.h. Used
// by all current KMI products.
uint16_t crcModern(const uint8_t *p, uint16_t len)
{
    uint16_t crc = 0;
    return crc_buf(&crc, p, len);
}

// Legacy (signed-char) KMI CRC, from MIDI_CPP's utils_crc.h. The pre-bootloader
// v93-era 8051 firmware computed its CRC with a signed char, so bytes >= 0x80
// were sign-extended before the XOR; MIDI_CPP's unsigned crc_byte() does not
// reproduce these images' checksums on any high-bit byte. See crc_byte_signed()
// in utils_crc.h for the full rationale.
uint16_t crcLegacySigned(const uint8_t *p, uint16_t len)
{
    uint16_t crc = 0;
    return crc_buf_signed(&crc, p, len);
}

// Decode one KMI packet starting at wire[pktStart] (which must be
// SX_PACKET_START == 0x01). Returns the decoded bytes (preamble + payload +
// continuation-length + payload-crc, plus up to 6 trailing flush bytes) and
// the number of wire bytes the packet occupied after the 0x01. Uses
// MIDI_CPP's decode primitives verbatim.
bool decodePacket(SysExMessageRX &rx, const std::vector<uint8_t> &wire,
                  std::size_t pktStart, std::vector<uint8_t> &decoded,
                  std::size_t &wireConsumed)
{
    rx.init_decode();
    decoded.clear();

    std::size_t i = pktStart + 1; // skip the raw 0x01
    uint8_t v;

    // Decode the 6-byte preamble first so we can read the length field.
    while (decoded.size() < sizeof(PACKET_PREAMBLE))
    {
        if (i >= wire.size())
            return false;
        rx.decode_put(wire[i++]);
        while (rx.decode_get(&v))
            decoded.push_back(v);
    }

    const uint16_t lengthField = be16(decoded, 2); // bytes 2..3 = length (BE)
    const std::size_t totalDecoded = sizeof(PACKET_PREAMBLE) + lengthField;

    while (decoded.size() < totalDecoded)
    {
        if (i >= wire.size())
            return false;
        rx.decode_put(wire[i++]);
        while (rx.decode_get(&v))
            decoded.push_back(v);
    }

    wireConsumed = i - (pktStart + 1);
    return true;
}

// Verify a decoded packet's preamble CRC (over category+type+length = 4 bytes,
// compared to the following 2 crc bytes) and, if it has a payload, its payload
// CRC (over payload + continuation-length, compared to the trailing 2 bytes) -
// exactly the coverage the firmware's packet_data_process()/MIDI_CPP's
// testDecodedCRC() use. Legacy images are checked with the signed-char CRC
// (crcLegacySigned); the modern unsigned CRC is accepted too so this same
// routine also validates a modern-encoded image without a special case.
void verifyCrcs(const std::vector<uint8_t> &decoded, uint16_t lengthField,
                bool &preCrcOk, bool &payloadCrcOk)
{
    const uint16_t preExpected = be16(decoded, 4);
    preCrcOk = (crcLegacySigned(decoded.data(), 4) == preExpected) ||
               (crcModern(decoded.data(), 4) == preExpected);

    payloadCrcOk = true;
    if (lengthField >= 4)
    {
        const uint16_t coverLen = static_cast<uint16_t>(lengthField - 2); // payload + cont-len
        const uint8_t *payload = decoded.data() + sizeof(PACKET_PREAMBLE);
        const uint16_t expected = be16(decoded, sizeof(PACKET_PREAMBLE) + coverLen);
        payloadCrcOk = (crcLegacySigned(payload, coverLen) == expected) ||
                       (crcModern(payload, coverLen) == expected);
    }
}

} // namespace

std::size_t LegacyFirmwareImage::totalSectors() const
{
    std::size_t n = 0;
    for (std::size_t i = 0; i < banks.size(); ++i)
        n += banks[i].sectors.size();
    return n;
}

bool LegacyFirmwareImage::allCrcOk() const
{
    for (std::size_t bi = 0; bi < banks.size(); ++bi)
    {
        if (!banks[bi].headerCrcOk)
            return false;
        for (std::size_t si = 0; si < banks[bi].sectors.size(); ++si)
            if (!banks[bi].sectors[si].blockHeaderCrcOk || !banks[bi].sectors[si].dataCrcOk)
                return false;
    }
    return true;
}

LegacyFirmwareImage decodeLegacyFirmwareSyx(const std::vector<uint8_t> &bytes)
{
    LegacyFirmwareImage img;

    if (bytes.size() < kOuterHeaderLen + 2 || bytes.front() != 0xF0)
    {
        img.error = "Not a SysEx file (missing leading 0xF0) or too short.";
        return img;
    }
    if (bytes.back() != 0xF7)
    {
        img.error = "File does not end in 0xF7 - legacy firmware must be a single F0..F7 message.";
        return img;
    }

    img.outerHeaderWire.assign(bytes.begin(), bytes.begin() + kOuterHeaderLen);

    SysExMessageRX rx;

    Bank *curBank = 0;
    std::vector<uint8_t> decoded;
    std::size_t pos = kOuterHeaderLen;

    while (pos < bytes.size())
    {
        // Skip inter-packet zero padding; stop at the terminating F7.
        while (pos < bytes.size() && bytes[pos] != 0x01)
        {
            if (bytes[pos] == 0xF7)
            {
                pos = bytes.size();
                break;
            }
            ++pos;
        }
        if (pos >= bytes.size())
            break;

        const std::size_t pktStart = pos;
        std::size_t wireConsumed = 0;
        if (!decodePacket(rx, bytes, pktStart, decoded, wireConsumed))
        {
            img.error = "Truncated/undecodable packet near byte offset " + std::to_string(pktStart) + ".";
            return img;
        }

        const uint8_t category = decoded[0];
        const uint8_t type = decoded[1];
        const uint16_t lengthField = be16(decoded, 2);
        (void)category;

        bool preCrcOk = false, payloadCrcOk = false;
        verifyCrcs(decoded, lengthField, preCrcOk, payloadCrcOk);

        // Original wire bytes of this packet (0x01 + encoded body + flush pad).
        std::vector<uint8_t> rawPacket(bytes.begin() + pktStart,
                                       bytes.begin() + pktStart + 1 + wireConsumed);

        const std::size_t payloadLen = (lengthField >= 4) ? (lengthField - 4) : 0;

        if (type == BLOCK_TYPE_FW_HEADER)
        {
            Bank bank;
            if (payloadLen >= 8)
            {
                bank.bank = decoded[6 + 0];
                bank.blockNumLast = decoded[6 + 1];
                bank.buildNumber = be16(decoded, 6 + 2);
                bank.imageLength = be16(decoded, 6 + 4);
                bank.imageCrc = be16(decoded, 6 + 6);
            }
            if (payloadLen >= 13)
            {
                // versionString[20] begins after the 12-byte fixed struct.
                for (std::size_t k = 6 + 12; k < 6 + payloadLen && decoded[k] != 0; ++k)
                    bank.version.push_back(static_cast<char>(decoded[k]));
            }
            bank.headerWire = rawPacket;
            bank.headerRawBegin = pktStart;
            bank.headerCrcOk = preCrcOk && payloadCrcOk;
            img.banks.push_back(bank);
            curBank = &img.banks.back();
        }
        else if (type == BLOCK_TYPE_FW_BLOCK_HEADER)
        {
            if (!curBank)
            {
                img.error = "FW_BLOCK_HEADER encountered before any FW_HEADER.";
                return img;
            }
            Sector sec;
            if (payloadLen >= 3)
            {
                sec.blockNum = decoded[6 + 0];
                sec.declaredLength = be16(decoded, 6 + 1);
            }
            sec.blockHeaderWire = rawPacket;
            sec.blockHeaderRawBegin = pktStart;
            sec.blockHeaderCrcOk = preCrcOk && payloadCrcOk;
            curBank->sectors.push_back(sec);
        }
        else if (type == BLOCK_TYPE_FW_DATA)
        {
            if (!curBank || curBank->sectors.empty())
            {
                img.error = "FW_DATA encountered before its FW_BLOCK_HEADER.";
                return img;
            }
            Sector &sec = curBank->sectors.back();
            sec.dataWire = rawPacket;
            sec.dataRawBegin = pktStart;
            sec.dataLength = static_cast<uint16_t>(payloadLen);
            sec.dataCrcOk = preCrcOk && payloadCrcOk;
        }
        // Other packet types (e.g. MISC_INFO) are ignored for the update stream.

        pos = pktStart + 1 + wireConsumed;
    }

    if (img.banks.empty())
    {
        img.error = "No FW_HEADER packets found - not a legacy firmware image?";
        return img;
    }

    // Tile the source file into raw-with-padding spans. Every stored packet's
    // 0x01 offset, in file (== send) order, is a cut point; each span runs from
    // its own offset to the next packet's offset (the last runs to EOF, so it
    // carries the terminating F7). outerHeaderRaw is the prefix before the first
    // packet. Concatenating outerHeaderRaw + every span reproduces the file
    // byte-for-byte, so sending the Raw spans is byte-exact with the original.
    {
        std::vector<std::pair<std::size_t, std::vector<uint8_t> *>> spans;
        for (std::size_t bi = 0; bi < img.banks.size(); ++bi)
        {
            Bank &bank = img.banks[bi];
            spans.push_back(std::make_pair(bank.headerRawBegin, &bank.headerRaw));
            for (std::size_t si = 0; si < bank.sectors.size(); ++si)
            {
                Sector &sec = bank.sectors[si];
                spans.push_back(std::make_pair(sec.blockHeaderRawBegin, &sec.blockHeaderRaw));
                if (!sec.dataWire.empty())
                    spans.push_back(std::make_pair(sec.dataRawBegin, &sec.dataRaw));
            }
        }

        const std::size_t firstBegin = spans.empty() ? bytes.size() : spans.front().first;
        img.outerHeaderRaw.assign(bytes.begin(), bytes.begin() + firstBegin);
        for (std::size_t k = 0; k < spans.size(); ++k)
        {
            const std::size_t begin = spans[k].first;
            const std::size_t end = (k + 1 < spans.size()) ? spans[k + 1].first : bytes.size();
            spans[k].second->assign(bytes.begin() + begin, bytes.begin() + end);
        }
    }

    img.ok = true;
    return img;
}

void printLegacyImageManifest(const LegacyFirmwareImage &img)
{
    if (!img.ok)
    {
        std::cout << "Decode FAILED: " << img.error << "\n";
        return;
    }

    std::cout << "Legacy firmware image: " << img.banks.size() << " bank(s), "
              << img.totalSectors() << " sector(s) total. "
              << "All CRCs " << (img.allCrcOk() ? "VALID" : "!! SOME INVALID !!") << ".\n";

    for (std::size_t bi = 0; bi < img.banks.size(); ++bi)
    {
        const Bank &b = img.banks[bi];
        std::size_t dataSum = 0;
        uint16_t badBlk = 0, badDat = 0;
        for (std::size_t si = 0; si < b.sectors.size(); ++si)
        {
            dataSum += b.sectors[si].dataLength;
            if (!b.sectors[si].blockHeaderCrcOk) ++badBlk;
            if (!b.sectors[si].dataCrcOk) ++badDat;
        }
        std::printf("\n== BANK %u  build=%u  header.length=%u  img_crc=0x%04X  "
                    "block_num_last=%u  ver='%s'  [headerCRC %s]\n",
                    b.bank, b.buildNumber, b.imageLength, b.imageCrc, b.blockNumLast,
                    b.version.c_str(), b.headerCrcOk ? "ok" : "BAD");
        std::printf("   %zu sectors, block_num %u..%u, sum(data)=%zu bytes%s\n",
                    b.sectors.size(),
                    b.sectors.empty() ? 0 : b.sectors.front().blockNum,
                    b.sectors.empty() ? 0 : b.sectors.back().blockNum,
                    dataSum,
                    (dataSum == b.imageLength) ? " (matches header.length)" : " (!! != header.length)");
        if (badBlk || badDat)
            std::printf("   !! CRC failures: %u block-header, %u data\n", badBlk, badDat);
        // first + last couple of sectors for a sanity glance
        for (std::size_t si = 0; si < b.sectors.size(); ++si)
        {
            if (si == 3 && b.sectors.size() > 5)
            {
                std::printf("   ...\n");
                si = b.sectors.size() - 2;
            }
            const Sector &s = b.sectors[si];
            std::printf("   block %3u: declared=%4u data=%4u  blkHdrCRC=%s dataCRC=%s\n",
                        s.blockNum, s.declaredLength, s.dataLength,
                        s.blockHeaderCrcOk ? "ok" : "BAD", s.dataCrcOk ? "ok" : "BAD");
        }
    }
}

} // namespace blupgrade
