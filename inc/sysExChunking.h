#ifndef SYSEX_CHUNKING_H
#define SYSEX_CHUNKING_H

//
//  sysExChunking.h
//
//  Shared F0...F7 SysEx message-boundary detection, used by both the raw
//  manual send path (SendSysEx.cpp) and the automatic-update path
//  (kmiDevice.cpp) so a Hex_to_SysEx-chunked .syx file (many independently
//  framed messages concatenated together) is only ever detected one way.
//
//  SPDX-License-Identifier: MIT
//

#include <cstddef>
#include <vector>

struct SysExChunk
{
    std::size_t offset;
    std::size_t length;
};

// Scans a raw byte buffer for complete 0xF0...0xF7 SysEx messages. Safe even
// on a single-message (historical, non-chunked) file: that just yields one
// chunk covering the whole buffer. A trailing, unterminated 0xF0 (truncated
// file) is silently ignored rather than treated as a chunk.
inline std::vector<SysExChunk> findSysExChunks(const std::vector<unsigned char> &bytes)
{
    std::vector<SysExChunk> chunks;
    std::size_t i = 0;
    while (i < bytes.size())
    {
        if (bytes[i] != 0xF0)
        {
            ++i;
            continue;
        }
        const std::size_t start = i;
        std::size_t j = i + 1;
        while (j < bytes.size() && bytes[j] != 0xF7)
            ++j;
        if (j >= bytes.size())
            break; // trailing, unterminated F0 - not a complete message, ignore it

        chunks.push_back(SysExChunk{start, j - start + 1});
        i = j + 1;
    }
    return chunks;
}

#endif // SYSEX_CHUNKING_H
