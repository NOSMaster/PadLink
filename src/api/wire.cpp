// SPDX-License-Identifier: GPL-2.0-or-later
#include "api/wire.h"

#include <cstring>

namespace padlink::wire {

namespace {

void put_le(Bytes& out, uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i)
        out.push_back(uint8_t(v >> (8 * i)));
}

uint64_t get_le(const uint8_t*& p, int bytes)
{
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i)
        v |= uint64_t(*p++) << (8 * i);
    return v;
}

void put_float(Bytes& out, float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, 4);  // IEEE-754 bits, little-endian on the wire
    put_le(out, bits, 4);
}

float get_float(const uint8_t*& p)
{
    const uint32_t bits = uint32_t(get_le(p, 4));
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

constexpr size_t kStateSize = 2 + 4 + 16 + 1 + 4 + 2 + 6 + 12 + 3;

} // namespace

Bytes message(Type type, ByteView body)
{
    Bytes out;
    out.reserve(5 + body.size());
    put_le(out, body.size() + 1, 4);
    out.push_back(uint8_t(type));
    put_bytes(out, body);
    return out;
}

std::optional<Message> take_message(Bytes& buffer, bool& bad)
{
    bad = false;
    if (buffer.size() < 5)
        return std::nullopt;
    const uint32_t length = read_le32(buffer.data());
    if (length == 0 || length > kMaxMessage) {
        bad = true;
        return std::nullopt;
    }
    if (buffer.size() < 4 + size_t(length))
        return std::nullopt;
    Message m{Type(buffer[4]), Bytes(buffer.begin() + 5, buffer.begin() + 4 + length)};
    buffer.erase(buffer.begin(), buffer.begin() + 4 + length);
    return m;
}

Bytes encode_state(const GamepadState& s)
{
    Bytes out;
    put_le(out, s.sequence, 2);
    put_le(out, s.buttons, 4);
    for (float f : {s.left_x, s.left_y, s.right_x, s.right_y})
        put_float(out, f);
    out.push_back(s.touch_pressed);
    put_le(out, uint16_t(s.touch_x), 2);
    put_le(out, uint16_t(s.touch_y), 2);
    put_le(out, s.touch_pressure, 2);
    for (int16_t a : s.accel)
        put_le(out, uint16_t(a), 2);
    for (int32_t g : s.gyro)
        put_le(out, uint32_t(g), 4);
    out.push_back(s.battery);
    out.push_back(s.volume);
    out.push_back(s.power_status);
    return out;
}

std::optional<GamepadState> decode_state(ByteView body)
{
    if (body.size() != kStateSize)
        return std::nullopt;
    const uint8_t* p = body.data();
    GamepadState s;
    s.sequence = uint16_t(get_le(p, 2));
    s.buttons = uint32_t(get_le(p, 4));
    s.left_x = get_float(p);
    s.left_y = get_float(p);
    s.right_x = get_float(p);
    s.right_y = get_float(p);
    s.touch_pressed = *p++ != 0;
    s.touch_x = int16_t(get_le(p, 2));
    s.touch_y = int16_t(get_le(p, 2));
    s.touch_pressure = uint16_t(get_le(p, 2));
    for (auto& a : s.accel)
        a = int16_t(get_le(p, 2));
    for (auto& g : s.gyro)
        g = int32_t(uint32_t(get_le(p, 4)));
    s.battery = *p++;
    s.volume = *p++;
    s.power_status = *p++;
    return s;
}

} // namespace padlink::wire
