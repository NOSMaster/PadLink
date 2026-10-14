// SPDX-License-Identifier: GPL-2.0-or-later
// Small helpers for reading/writing integers with an explicit byte order.
//
// The GamePad protocol mixes big-endian (BE) and little-endian (LE) fields,
// so we never cast structs over buffers; every field goes through one of
// these functions instead.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace padlink {

using Bytes = std::vector<uint8_t>;
using ByteView = std::span<const uint8_t>;

inline uint16_t read_be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint16_t read_le16(const uint8_t* p) { return uint16_t(p[1] << 8 | p[0]); }
inline uint32_t read_be32(const uint8_t* p)
{
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint32_t read_le32(const uint8_t* p)
{
    return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}

inline void put_be16(Bytes& out, uint16_t v)
{
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}
inline void put_be32(Bytes& out, uint32_t v)
{
    put_be16(out, uint16_t(v >> 16));
    put_be16(out, uint16_t(v));
}
inline void put_bytes(Bytes& out, ByteView data) { out.insert(out.end(), data.begin(), data.end()); }
inline void put_string(Bytes& out, const std::string& s) { out.insert(out.end(), s.begin(), s.end()); }

// "0a1b..." (lowercase, no separators).
inline std::string to_hex(ByteView data)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(data.size() * 2);
    for (uint8_t b : data) {
        s.push_back(digits[b >> 4]);
        s.push_back(digits[b & 0x0f]);
    }
    return s;
}

} // namespace padlink
