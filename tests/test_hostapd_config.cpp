// SPDX-License-Identifier: GPL-2.0-or-later
#include "doctest.h"
#include "wifi/hostapd_config.h"

using namespace padlink;

namespace {
const ApSettings kAp{"wlan0", "/run/padlink/hostapd", *MacAddress::parse("02:12:34:56:78:ef"), 36};

bool has_line(const std::string& conf, const std::string& line)
{
    return conf.find("\n" + line + "\n") != std::string::npos;
}
} // namespace

TEST_CASE("pairing config is an open WPS registrar with our credential")
{
    const std::string c = pairing_config(kAp, "/run/padlink/cred.bin");
    CHECK(has_line(c, "interface=wlan0"));
    CHECK(has_line(c, "bssid=02:12:34:56:78:ef"));
    CHECK(has_line(c, "hw_mode=a"));
    CHECK(has_line(c, "channel=36"));
    CHECK(has_line(c, "ssid=WiiU0212345678e0212345678ef_STA1"));
    CHECK(has_line(c, "wps_state=2"));
    CHECK(has_line(c, "skip_cred_build=1"));
    CHECK(has_line(c, "extra_cred=/run/padlink/cred.bin"));
    CHECK(c.find("wpa=") == std::string::npos);  // open network
}

TEST_CASE("runtime config is hidden WPA2-PSK CCMP")
{
    const std::string psk(64, 'b');
    const std::string c = runtime_config(kAp, "WiiU0212345678ef", psk);
    CHECK(has_line(c, "ssid=WiiU0212345678ef"));
    CHECK(has_line(c, "ignore_broadcast_ssid=2"));
    CHECK(has_line(c, "wpa=2"));
    CHECK(has_line(c, "wpa_key_mgmt=WPA-PSK"));
    CHECK(has_line(c, "rsn_pairwise=CCMP"));
    CHECK(has_line(c, "wpa_psk=" + psk));
    CHECK(c.find("wps_state") == std::string::npos);
}
