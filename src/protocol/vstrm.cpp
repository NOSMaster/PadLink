// SPDX-License-Identifier: GPL-2.0-or-later
#include "protocol/vstrm.h"

#include <algorithm>

namespace padlink {

namespace {

// Extension options as ordered by a real console [observed]:
// 80 = IDR, 82 00 = 59.94 Hz, 83 = force decoding, 85 06 = 6 MB rows/chunk.
constexpr std::array<uint8_t, 8> kIdrOptions{0x80, 0x82, 0x00, 0x83, 0x85, 0x06, 0x00, 0x00};
constexpr std::array<uint8_t, 8> kPOptions{0x82, 0x00, 0x83, 0x85, 0x06, 0x00, 0x00, 0x00};

void put_le32(Bytes& out, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(uint8_t(v >> (8 * i)));
}

} // namespace

Bytes build_vstrm_packet(const VstrmHeader& h, ByteView payload)
{
    Bytes out;
    out.reserve(kVstrmHeaderSize + payload.size());
    const uint16_t seq = h.sequence & 0x3ff;
    const size_t size = std::min(payload.size(), size_t(0x7ff));
    out.push_back(uint8_t(0xf0 | (seq >> 8)));  // magic 0xF, packet type 0, seq[9:8]
    out.push_back(uint8_t(seq));
    out.push_back(uint8_t((h.init ? 0x80 : 0) | (h.frame_begin ? 0x40 : 0) | (h.chunk_end ? 0x20 : 0) |
                          (h.frame_end ? 0x10 : 0) | 0x08 /* has timestamp */ | (size >> 8)));
    out.push_back(uint8_t(size));
    put_be32(out, h.timestamp);
    const auto& opts = h.idr ? kIdrOptions : kPOptions;
    out.insert(out.end(), opts.begin(), opts.end());
    out.insert(out.end(), payload.begin(), payload.begin() + ptrdiff_t(size));
    return out;
}

std::optional<ParsedVstrm> parse_vstrm_packet(ByteView p)
{
    if (p.size() < kVstrmHeaderSize || (p[0] >> 4) != 0xf)
        return std::nullopt;
    const size_t size = size_t(p[2] & 0x07) << 8 | p[3];
    if (p.size() != kVstrmHeaderSize + size)
        return std::nullopt;
    ParsedVstrm out;
    VstrmHeader& h = out.header;
    h.sequence = uint16_t((p[0] & 0x03) << 8 | p[1]);
    h.init = p[2] & 0x80;
    h.frame_begin = p[2] & 0x40;
    h.chunk_end = p[2] & 0x20;
    h.frame_end = p[2] & 0x10;
    h.timestamp = read_be32(&p[4]);
    for (size_t i = 8; i < 16 && p[i] != 0; ++i) {
        if (p[i] == 0x80)
            h.idr = true;
        else if (p[i] == 0x81 || p[i] == 0x82 || p[i] == 0x85)
            ++i;  // option with a 1-byte argument
    }
    out.payload.assign(p.begin() + kVstrmHeaderSize, p.end());
    return out;
}

std::vector<Bytes> packetize_frame(const std::array<Bytes, kVideoChunks>& chunks, bool idr, bool init,
                                   uint32_t timestamp, uint16_t& sequence)
{
    std::vector<Bytes> packets;
    for (size_t c = 0; c < kVideoChunks; ++c) {
        const Bytes& chunk = chunks[c];
        size_t offset = 0;
        do {
            const size_t n = std::min(kVstrmMaxPayload, chunk.size() - offset);
            VstrmHeader h;
            h.sequence = sequence;
            h.init = init;
            h.idr = idr;
            h.timestamp = timestamp;
            h.frame_begin = packets.empty();
            h.chunk_end = offset + n == chunk.size();
            h.frame_end = h.chunk_end && c == kVideoChunks - 1;
            packets.push_back(build_vstrm_packet(h, ByteView(chunk).subspan(offset, n)));
            sequence = (sequence + 1) & 0x3ff;
            offset += n;
        } while (offset < chunk.size());
    }
    return packets;
}

Bytes build_video_format(uint32_t timestamp)
{
    // Outer 8 bytes: type bit set, length deliberately 0, then bytes whose
    // meaning is unknown (taken from a fresh real-console capture) [observed].
    Bytes out{0x04, 0x00, 0x00, 0x00, 0x10, 0x10, 0x81, 0x08};
    put_le32(out, timestamp);
    put_le32(out, 16000);
    put_le32(out, 16000);
    put_le32(out, 21000);
    put_le32(out, 21000);
    put_le32(out, 0);
    return out;
}

} // namespace padlink
