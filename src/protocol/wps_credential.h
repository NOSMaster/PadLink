// SPDX-License-Identifier: GPL-2.0-or-later
// The WPS "Credential" attribute that tells the GamePad how to join the
// normal AP. hostapd sends these bytes verbatim (config: extra_cred=<file>).
#pragma once

#include "protocol/bytes.h"
#include "protocol/pairing.h"

#include <optional>
#include <string>

namespace padlink {

// WPS attribute type ids (WPS spec, all values big-endian on the wire).
namespace wps_attr {
inline constexpr uint16_t AuthType = 0x1003;
inline constexpr uint16_t Credential = 0x100e;
inline constexpr uint16_t EncrType = 0x100f;
inline constexpr uint16_t MacAddress = 0x1020;
inline constexpr uint16_t NetworkIndex = 0x1026;
inline constexpr uint16_t NetworkKey = 0x1027;
inline constexpr uint16_t Ssid = 0x1045;
} // namespace wps_attr

struct WpsCredential {
    std::string ssid;     // 1..32 bytes
    std::string psk_hex;  // exactly 64 hex characters (a raw 256-bit PSK)
    padlink::MacAddress ap_mac;
};

// Returns std::nullopt if the SSID or PSK is malformed.
std::optional<Bytes> build_wps_credential(const WpsCredential& cred);

} // namespace padlink
