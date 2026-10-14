// SPDX-License-Identifier: GPL-2.0-or-later
// VSTRM video packets (UDP 50020 -> 50120) and the ASTRM "video format"
// message sent before every frame (UDP 50021 -> 50121). PROTOCOL.md §4–5.
#pragma once

#include "protocol/bytes.h"

#include <array>
#include <optional>
#include <vector>

namespace padlink {

inline constexpr size_t kVstrmHeaderSize = 16;
inline constexpr size_t kVstrmMaxPayload = 1694;  // real-console maximum
inline constexpr size_t kVideoChunks = 5;          // 6 macroblock rows each

struct VstrmHeader {
    uint16_t sequence = 0;  // 10 bits
    bool init = false;      // first frame of the stream
    bool frame_begin = false;
    bool chunk_end = false;
    bool frame_end = false;
    bool idr = false;       // extension option 0x80
    uint32_t timestamp = 0; // AP TSF, µs
};

Bytes build_vstrm_packet(const VstrmHeader& h, ByteView payload);

struct ParsedVstrm {
    VstrmHeader header;
    Bytes payload;
};
std::optional<ParsedVstrm> parse_vstrm_packet(ByteView packet);

// Splits one encoded frame (5 chunks) into VSTRM packets. `sequence` is
// advanced (mod 1024) for every packet produced.
std::vector<Bytes> packetize_frame(const std::array<Bytes, kVideoChunks>& chunks, bool idr, bool init,
                                   uint32_t timestamp, uint16_t& sequence);

// The 32-byte video-format message (ASTRM type bit set).
Bytes build_video_format(uint32_t timestamp);

} // namespace padlink
