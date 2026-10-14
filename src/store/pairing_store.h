// SPDX-License-Identifier: GPL-2.0-or-later
// Remembers a paired GamePad: a small key=value text file, readable only by
// root (it contains the Wi-Fi PSK).
#pragma once

#include "protocol/pairing.h"

#include <optional>
#include <string>

namespace padlink {

struct PairingRecord {
    MacAddress ap_mac;
    std::string ssid;
    std::string psk_hex;
    int channel = 36;
    std::optional<MacAddress> gamepad_mac;  // learned at pairing or first connect
};

std::string serialize(const PairingRecord& rec);
std::optional<PairingRecord> deserialize(const std::string& text);

std::optional<PairingRecord> load_pairing(const std::string& path);
bool save_pairing(const std::string& path, const PairingRecord& rec, std::string& error);

} // namespace padlink
