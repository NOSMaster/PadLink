// SPDX-License-Identifier: GPL-2.0-or-later
// Pairing identifiers: MAC addresses, the 4-symbol code, SSIDs and WPS PIN.
// See PROTOCOL.md §1.4.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace padlink {

struct MacAddress {
    std::array<uint8_t, 6> bytes{};

    // Accepts "aa:bb:cc:dd:ee:ff" (any case). Returns std::nullopt on error.
    static std::optional<MacAddress> parse(std::string_view text);
    std::string to_string() const;  // "aa:bb:cc:dd:ee:ff"
    std::string to_hex() const;     // "aabbccddeeff"

    bool operator==(const MacAddress&) const = default;
};

// The 4 card symbols shown during pairing. Each digit is 0..3.
enum class Symbol : uint8_t { Spade = 0, Heart = 1, Diamond = 2, Club = 3 };

struct PairingCode {
    std::array<uint8_t, 4> digits{};  // each 0..3

    // 2 bits per digit, first digit in the top bits.
    static PairingCode from_byte(uint8_t packed);
    uint8_t to_byte() const;

    std::string wps_pin() const;      // e.g. "00235678"
    std::string symbols() const;      // e.g. "♠ ♠ ♦ ♣"
    std::string names() const;        // e.g. "spade spade diamond club"
};

// The code is derived from the AP MAC's last byte, so the MAC embedded in the
// pairing SSID equals the real BSSID (as on a real console).
PairingCode pairing_code_for(const MacAddress& ap_mac);

// "WiiU" + first 11 hex nibbles + all 12 nibbles + "_STA1".
std::string pairing_ssid(const MacAddress& ap_mac);

// SSID of the normal (runtime) AP, handed to the GamePad by WPS.
std::string runtime_ssid(const MacAddress& ap_mac);

} // namespace padlink
