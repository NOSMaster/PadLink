// SPDX-License-Identifier: GPL-2.0-or-later
#include "doctest.h"
#include "clock/tsf_clock.h"

using namespace padlink;

namespace {

// A fake radio: TSF = offset + mono * (1 + ppm/1e6), plus an optional glitch.
struct FakeRadio {
    int64_t mono = 1000000;  // fake CLOCK_MONOTONIC, µs
    double ppm = 0;
    uint64_t offset = 5000000000ULL;
    int64_t glitch = 0;
    bool fail = false;
    int64_t read_cost = 50;  // µs consumed by each read

    TsfClock make()
    {
        return TsfClock(
            [this]() -> std::optional<uint64_t> {
                mono += read_cost / 2;
                const auto value = uint64_t(double(offset) + double(mono) * (1 + ppm / 1e6)) + uint64_t(glitch);
                mono += read_cost / 2;
                if (fail)
                    return std::nullopt;
                return value;
            },
            [this] { return mono; });
    }
};

} // namespace

TEST_CASE("clock becomes healthy after two consistent samples")
{
    FakeRadio radio;
    auto clock = radio.make();
    CHECK_FALSE(clock.healthy());
    CHECK(clock.sample());
    CHECK_FALSE(clock.healthy());  // one sample can't be verified
    radio.mono += 100000;
    CHECK(clock.sample());
    CHECK(clock.healthy());
}

TEST_CASE("extrapolates between samples and truncates to 32 bits")
{
    FakeRadio radio;
    auto clock = radio.make();
    clock.sample();
    radio.mono += 100000;
    clock.sample();
    const uint64_t at_sample = clock.now_tsf();
    radio.mono += 1234;
    CHECK(clock.now_tsf() == at_sample + 1234);
    CHECK(clock.timestamp() == uint32_t(clock.now_tsf()));
}

TEST_CASE("measures drift in ppm")
{
    FakeRadio radio;
    radio.ppm = 20;
    auto clock = radio.make();
    for (int i = 0; i < 100; ++i) {
        clock.sample();
        radio.mono += 100000;
    }
    REQUIRE(clock.drift_ppm());
    CHECK(*clock.drift_ppm() == doctest::Approx(20).epsilon(0.05));
}

TEST_CASE("a jump makes the clock unhealthy until it is confirmed again")
{
    FakeRadio radio;
    auto clock = radio.make();
    clock.sample();
    radio.mono += 100000;
    clock.sample();
    REQUIRE(clock.healthy());

    radio.glitch = 50000;  // TSF reset / wrong register
    radio.mono += 100000;
    CHECK_FALSE(clock.sample());
    CHECK_FALSE(clock.healthy());
    radio.mono += 100000;
    CHECK(clock.sample());  // consistent with the new value
    CHECK(clock.healthy());
}

TEST_CASE("failed, slow and stale reads are unhealthy")
{
    FakeRadio radio;
    auto clock = radio.make();
    clock.sample();
    radio.mono += 100000;
    clock.sample();
    REQUIRE(clock.healthy());

    radio.fail = true;
    CHECK_FALSE(clock.sample());
    CHECK_FALSE(clock.healthy());

    radio.fail = false;
    radio.mono += 100000;
    CHECK(clock.sample());
    CHECK(clock.healthy());
    radio.read_cost = 10000;
    CHECK_FALSE(clock.sample());
    radio.read_cost = 50;

    radio.mono += 100000;
    clock.sample();
    clock.sample();
    REQUIRE(clock.healthy());
    radio.mono += 2000000;  // no samples for 2 s
    CHECK_FALSE(clock.healthy());
}

TEST_CASE("rtw89 read_reg output is parsed strictly")
{
    CHECK(parse_rtw89_read_reg("get 4 bytes at 0x0000c438=0x1234abcd\n", 0xc438) == 0x1234abcdu);
    CHECK_FALSE(parse_rtw89_read_reg("get 4 bytes at 0x0000c43c=0x1", 0xc438));   // wrong register
    CHECK_FALSE(parse_rtw89_read_reg("get 2 bytes at 0x0000c438=0x1", 0xc438));   // wrong size
    CHECK_FALSE(parse_rtw89_read_reg("get 4 bytes at 0x0000c438=0xdeadbeef", 0xc438));
    CHECK_FALSE(parse_rtw89_read_reg("garbage", 0xc438));
}
