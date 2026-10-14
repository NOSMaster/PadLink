// SPDX-License-Identifier: GPL-2.0-or-later
#include "doctest.h"
#include "protocol/wps_credential.h"

using namespace padlink;

namespace {
const std::string kPsk(64, 'a');
}

TEST_CASE("credential is one Credential TLV with the expected attributes in order")
{
    const WpsCredential cred{"WiiU0212345678ef", kPsk, *MacAddress::parse("02:12:34:56:78:ef")};
    const auto blob = build_wps_credential(cred);
    REQUIRE(blob);
    const Bytes& b = *blob;

    // Outer TLV header: type 0x100e, length = everything after it.
    REQUIRE(b.size() > 4);
    CHECK(read_be16(&b[0]) == 0x100e);
    CHECK(read_be16(&b[2]) == b.size() - 4);

    // Walk the inner TLVs.
    std::vector<uint16_t> types;
    size_t i = 4;
    while (i + 4 <= b.size()) {
        const uint16_t type = read_be16(&b[i]);
        const uint16_t len = read_be16(&b[i + 2]);
        REQUIRE(i + 4 + len <= b.size());
        types.push_back(type);
        if (type == 0x1045) CHECK(std::string(b.begin() + i + 4, b.begin() + i + 4 + len) == cred.ssid);
        if (type == 0x1003) CHECK(read_be16(&b[i + 4]) == 0x0020);  // WPA2-PSK
        if (type == 0x100f) CHECK(read_be16(&b[i + 4]) == 0x0008);  // AES
        if (type == 0x1027) CHECK(len == 64);
        if (type == 0x1020) CHECK(Bytes(b.begin() + i + 4, b.begin() + i + 10) == Bytes{0x02, 0x12, 0x34, 0x56, 0x78, 0xef});
        i += 4 + len;
    }
    CHECK(i == b.size());
    CHECK(types == std::vector<uint16_t>{0x1026, 0x1045, 0x1003, 0x100f, 0x1027, 0x1020});
}

TEST_CASE("malformed credentials are rejected")
{
    const auto mac = *MacAddress::parse("02:12:34:56:78:ef");
    CHECK_FALSE(build_wps_credential({"", kPsk, mac}));
    CHECK_FALSE(build_wps_credential({std::string(33, 'x'), kPsk, mac}));
    CHECK_FALSE(build_wps_credential({"ok", "short", mac}));
    CHECK_FALSE(build_wps_credential({"ok", std::string(64, 'g'), mac}));
}
