// SPDX-License-Identifier: GPL-2.0-or-later
// Generates hostapd configuration text for the two AP modes.
#pragma once

#include "protocol/pairing.h"

#include <string>

namespace padlink {

struct ApSettings {
    std::string interface;          // e.g. "wlan0"
    std::string ctrl_dir;           // hostapd control socket directory
    MacAddress ap_mac;
    int channel = 36;
};

// Open AP named WiiU…_STA1 acting as WPS registrar. `credential_path` is a
// file with the bytes from build_wps_credential().
std::string pairing_config(const ApSettings& ap, const std::string& credential_path);

// Normal WPA2-PSK AP the paired GamePad joins on every boot.
std::string runtime_config(const ApSettings& ap, const std::string& ssid, const std::string& psk_hex);

} // namespace padlink
