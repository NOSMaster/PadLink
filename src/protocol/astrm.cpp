// SPDX-License-Identifier: GPL-2.0-or-later
#include "protocol/astrm.h"

namespace padlink {

namespace {
constexpr uint16_t kFormatPcm48k = 1;
}

Bytes build_audio_packet(const AstrmAudioHeader& h, ByteView pcm)
{
    // First 16 bits, big-endian: format(3) mono(1) vibrate(1) type(1) seq(10).
    const uint16_t word = uint16_t(kFormatPcm48k << 13 | (h.vibrate ? 1 << 11 : 0) | (h.sequence & 0x3ff));
    Bytes out;
    out.reserve(8 + pcm.size());
    put_be16(out, word);
    put_be16(out, uint16_t(pcm.size()));
    for (int i = 0; i < 4; ++i)  // timestamp: little-endian [observed]
        out.push_back(uint8_t(h.timestamp >> (8 * i)));
    put_bytes(out, pcm);
    return out;
}

std::optional<ParsedAudio> parse_astrm_packet(ByteView p)
{
    if (p.size() < 8)
        return std::nullopt;
    const uint16_t word = read_be16(&p[0]);
    ParsedAudio a;
    a.format = word >> 13;
    a.mono = word & (1 << 12);
    a.header.vibrate = word & (1 << 11);
    a.is_video_format = word & (1 << 10);
    a.header.sequence = word & 0x3ff;
    a.header.timestamp = read_le32(&p[4]);
    const uint16_t size = read_be16(&p[2]);
    if (!a.is_video_format && p.size() != 8u + size)
        return std::nullopt;
    a.payload.assign(p.begin() + 8, p.end());
    return a;
}

} // namespace padlink
