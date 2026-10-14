// SPDX-License-Identifier: GPL-2.0-or-later
// ASTRM audio packets: console UDP 50021 -> GamePad 50121 (PROTOCOL.md §5).
#pragma once

#include "padlink/padlink.h"  // kAudioRate
#include "protocol/bytes.h"

#include <optional>

namespace padlink {

inline constexpr int kAudioChannels = 2;
inline constexpr size_t kAudioFramesPerPacket = 416;  // 8.667 ms
inline constexpr size_t kAudioPayloadBytes = kAudioFramesPerPacket * kAudioChannels * 2;  // 1664

struct AstrmAudioHeader {
    uint16_t sequence = 0;   // 10 bits
    bool vibrate = false;    // GamePad rumble motor on while set
    uint32_t timestamp = 0;  // AP TSF, µs
};

// `pcm`: interleaved signed 16-bit little-endian stereo, any length (normally
// kAudioPayloadBytes). Format: PCM 48 kHz.
Bytes build_audio_packet(const AstrmAudioHeader& h, ByteView pcm);

struct ParsedAudio {
    AstrmAudioHeader header;
    int format = 0;     // 1 = PCM 48 kHz
    bool mono = false;
    bool is_video_format = false;
    Bytes payload;
};
std::optional<ParsedAudio> parse_astrm_packet(ByteView packet);

} // namespace padlink
