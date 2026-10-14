// SPDX-License-Identifier: GPL-2.0-or-later
#include "doctest.h"
#include "store/pairing_store.h"

#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

using namespace padlink;

namespace {
PairingRecord sample()
{
    PairingRecord r;
    r.ap_mac = *MacAddress::parse("02:12:34:56:78:ef");
    r.ssid = "WiiU0212345678ef";
    r.psk_hex = std::string(64, 'c');
    r.channel = 40;
    return r;
}
} // namespace

TEST_CASE("pairing record round-trips through text")
{
    PairingRecord r = sample();
    auto back = deserialize(serialize(r));
    REQUIRE(back);
    CHECK(back->ap_mac == r.ap_mac);
    CHECK(back->ssid == r.ssid);
    CHECK(back->psk_hex == r.psk_hex);
    CHECK(back->channel == 40);
    CHECK_FALSE(back->gamepad_mac);

    r.gamepad_mac = MacAddress::parse("40:d2:8a:ab:90:00");
    back = deserialize(serialize(r));
    REQUIRE(back);
    CHECK(back->gamepad_mac == r.gamepad_mac);
}

TEST_CASE("broken pairing files are rejected")
{
    CHECK_FALSE(deserialize(""));
    CHECK_FALSE(deserialize("ap_mac=nope\nssid=x\npsk=" + std::string(64, 'c') + "\nchannel=36\n"));
    CHECK_FALSE(deserialize("ap_mac=02:12:34:56:78:ef\nssid=x\npsk=short\nchannel=36\n"));
    CHECK_FALSE(deserialize("ap_mac=02:12:34:56:78:ef\nssid=x\npsk=" + std::string(64, 'c') + "\nchannel=abc\n"));
    CHECK_FALSE(deserialize("garbage line\n"));
}

TEST_CASE("saved file is private (mode 0600) and loads back")
{
    const auto dir = std::filesystem::temp_directory_path() / ("padlink-test-" + std::to_string(::getpid()));
    const std::string path = (dir / "sub" / "pairing.conf").string();
    std::string error;
    REQUIRE(save_pairing(path, sample(), error));
    struct stat st{};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    const auto back = load_pairing(path);
    REQUIRE(back);
    CHECK(back->channel == 40);
    std::filesystem::remove_all(dir);
}
