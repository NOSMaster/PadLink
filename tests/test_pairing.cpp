// SPDX-License-Identifier: GPL-2.0-or-later
#include "doctest.h"
#include "protocol/pairing.h"

using namespace padlink;

TEST_CASE("MAC addresses parse and format")
{
    const auto mac = MacAddress::parse("02:12:34:56:78:EF");
    REQUIRE(mac);
    CHECK(mac->to_string() == "02:12:34:56:78:ef");
    CHECK(mac->to_hex() == "0212345678ef");
    CHECK_FALSE(MacAddress::parse("02:12:34:56:78"));
    CHECK_FALSE(MacAddress::parse("c4-13-75-f4-03-ef"));
    CHECK_FALSE(MacAddress::parse("zz:13:75:f4:03:ef"));
}

TEST_CASE("pairing code packs 2 bits per symbol")
{
    // ♠♠♦♣ = 0,0,2,3 -> 0b00'00'10'11
    const auto code = PairingCode::from_byte(0x0b);
    CHECK(code.digits == std::array<uint8_t, 4>{0, 0, 2, 3});
    CHECK(code.to_byte() == 0x0b);
    CHECK(code.symbols() == "♠ ♠ ♦ ♣");
    CHECK(code.names() == "spade spade diamond club");
    for (int b = 0; b < 256; ++b)
        CHECK(PairingCode::from_byte(uint8_t(b)).to_byte() == b);
}

TEST_CASE("WPS PIN is the symbol digits followed by 5678 (libdrc example)")
{
    CHECK(PairingCode::from_byte(0x0b).wps_pin() == "00235678");
    CHECK(PairingCode::from_byte(0xff).wps_pin() == "33335678");
}

TEST_CASE("code comes from the last MAC byte; SSIDs embed the MAC")
{
    const auto mac = *MacAddress::parse("40:d2:8a:bf:fc:a8");
    CHECK(pairing_code_for(mac).to_byte() == 0xa8);  // 10 10 10 00 = ♦♦♦♠
    CHECK(pairing_code_for(mac).wps_pin() == "22205678");
    CHECK(pairing_ssid(mac) == "WiiU40d28abffca40d28abffca8_STA1");
    CHECK(runtime_ssid(mac) == "WiiU40d28abffca8");
}
