// SPDX-License-Identifier: GPL-2.0-or-later
// The local socket protocol between padlinkd and apps (libpadlink).
//
// A stream of messages, each:  u32 LE length | u8 type | body (length-1 bytes)
// The first message in each direction is Hello with the protocol version.
#pragma once

#include "padlink/padlink.h"
#include "protocol/bytes.h"

#include <optional>

namespace padlink::wire {

inline constexpr uint32_t kVersion = 1;
inline constexpr size_t kMaxMessage = 1 << 20;  // 1 MiB (a video frame is 622080 bytes)
inline constexpr size_t kI420Size = 864 * 480 * 3 / 2;

enum class Type : uint8_t {
    Hello = 1,       // both: u32 version
    VideoI420 = 2,   // app -> daemon: kI420Size bytes
    AudioPcm = 3,    // app -> daemon: s16le stereo frames
    Rumble = 4,      // app -> daemon: u8 on
    PadState = 16,   // daemon -> app: u8 connected
    Input = 17,      // daemon -> app: GamepadState (see encode_state)
};

Bytes message(Type type, ByteView body);

// Splits complete messages off the front of `buffer`. Returns std::nullopt
// when no complete message is buffered yet; sets `bad` on a malformed one.
struct Message {
    Type type;
    Bytes body;
};
std::optional<Message> take_message(Bytes& buffer, bool& bad);

Bytes encode_state(const GamepadState& s);
std::optional<GamepadState> decode_state(ByteView body);

} // namespace padlink::wire
