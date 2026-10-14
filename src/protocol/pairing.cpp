// SPDX-License-Identifier: GPL-2.0-or-later
#include "protocol/pairing.h"

#include "protocol/bytes.h"

#include <cctype>

namespace padlink {

namespace {

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = char(std::tolower(static_cast<unsigned char>(c)));
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

constexpr std::array<const char*, 4> kSymbols{"♠", "♥", "♦", "♣"};
constexpr std::array<const char*, 4> kNames{"spade", "heart", "diamond", "club"};

} // namespace

std::optional<MacAddress> MacAddress::parse(std::string_view text)
{
    if (text.size() != 17)
        return std::nullopt;
    MacAddress mac;
    for (size_t i = 0; i < 6; ++i) {
        const int hi = hex_value(text[i * 3]);
        const int lo = hex_value(text[i * 3 + 1]);
        if (hi < 0 || lo < 0 || (i < 5 && text[i * 3 + 2] != ':'))
            return std::nullopt;
        mac.bytes[i] = uint8_t(hi << 4 | lo);
    }
    return mac;
}

std::string MacAddress::to_hex() const { return padlink::to_hex(bytes); }

std::string MacAddress::to_string() const
{
    const std::string hex = to_hex();
    std::string s;
    for (size_t i = 0; i < 6; ++i) {
        if (i) s.push_back(':');
        s.append(hex, i * 2, 2);
    }
    return s;
}

PairingCode PairingCode::from_byte(uint8_t packed)
{
    PairingCode code;
    for (int i = 0; i < 4; ++i)
        code.digits[i] = (packed >> (6 - 2 * i)) & 0x03;
    return code;
}

uint8_t PairingCode::to_byte() const
{
    uint8_t packed = 0;
    for (int i = 0; i < 4; ++i)
        packed |= uint8_t((digits[i] & 0x03) << (6 - 2 * i));
    return packed;
}

std::string PairingCode::wps_pin() const
{
    std::string pin;
    for (uint8_t d : digits)
        pin.push_back(char('0' + (d & 0x03)));
    return pin + "5678";  // fixed second half used by every Wii U
}

std::string PairingCode::symbols() const
{
    std::string s;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i) s += ' ';
        s += kSymbols[digits[i] & 0x03];
    }
    return s;
}

std::string PairingCode::names() const
{
    std::string s;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i) s += ' ';
        s += kNames[digits[i] & 0x03];
    }
    return s;
}

PairingCode pairing_code_for(const MacAddress& ap_mac) { return PairingCode::from_byte(ap_mac.bytes[5]); }

std::string pairing_ssid(const MacAddress& ap_mac)
{
    const std::string hex = ap_mac.to_hex();
    return "WiiU" + hex.substr(0, 11) + hex + "_STA1";
}

std::string runtime_ssid(const MacAddress& ap_mac) { return "WiiU" + ap_mac.to_hex(); }

} // namespace padlink
