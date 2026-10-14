// SPDX-License-Identifier: GPL-2.0-or-later
#include "doctest.h"
#include "protocol/input.h"

using namespace padlink;

namespace {

void le16(Bytes& b, size_t at, uint16_t v)
{
    b[at] = uint8_t(v);
    b[at + 1] = uint8_t(v >> 8);
}

// Touch coordinate word: 12-bit value + 3 extra bits at bits 12..14.
uint16_t touch_word(uint16_t value, uint8_t extra) { return uint16_t((value & 0x0fff) | (extra & 7) << 12); }

Bytes sample_packet()
{
    Bytes b(128, 0);
    b[0] = 0x12; b[1] = 0x34;              // sequence (BE)
    b[2] = 0x80; b[3] = 0x01;              // A + Sync (BE)
    b[80] = 0x20;                          // TV
    b[4] = power::AcPlugged | power::Charging;
    b[5] = 0x55;                           // battery
    le16(b, 6, 2050); le16(b, 8, 3200); le16(b, 10, 900); le16(b, 12, 2100);
    b[14] = 0x3f;                          // volume
    le16(b, 15, uint16_t(int16_t(-100))); le16(b, 17, 200); le16(b, 19, 4000);
    b[21] = 0xff; b[22] = 0xff; b[23] = 0xff;   // gyro[0] = -1
    b[24] = 0x00; b[25] = 0x00; b[26] = 0x40;   // gyro[1] = 0x400000
    b[27] = 0x01; b[28] = 0x00; b[29] = 0x80;   // gyro[2] = -0x7fffff
    b[83] = 0x01;                          // waiting for video
    b[127] = uint8_t(~0x2a);
    return b;
}

} // namespace

TEST_CASE("input report fields are decoded with the right byte order")
{
    const auto r = parse_input_report(sample_packet());
    REQUIRE(r);
    CHECK(r->sequence == 0x1234);
    CHECK(r->buttons == (btn::A | btn::Sync | btn::TV));
    CHECK(r->power_status == (power::AcPlugged | power::Charging));
    CHECK(r->battery == 0x55);
    CHECK(r->sticks == std::array<uint16_t, 4>{2050, 3200, 900, 2100});
    CHECK(r->volume == 0x3f);
    CHECK(r->accel == std::array<int16_t, 3>{-100, 200, 4000});
    CHECK(r->gyro == std::array<int32_t, 3>{-1, 0x400000, -0x7fffff});
    CHECK(r->waiting_for_video);
    CHECK(r->firmware_version == 0x2a);
    CHECK_FALSE(r->touch.pressed);
}

TEST_CASE("only 128-byte reports are accepted")
{
    CHECK_FALSE(parse_input_report(Bytes(127, 0)));
    CHECK_FALSE(parse_input_report(Bytes(129, 0)));
}

TEST_CASE("touch: 10 samples are averaged, pressure from 4 extra-bit fields")
{
    Bytes b = sample_packet();
    for (size_t i = 0; i < 10; ++i) {
        le16(b, 36 + 4 * i, touch_word(uint16_t(1000 + i * 2), 0));  // x: 1000..1018 -> avg 1009
        le16(b, 38 + 4 * i, touch_word(3000, 0));
    }
    // pressure = 0b101'011'010'001 = extras (p0.x=1, p0.y=2, p1.x=3, p1.y=5)
    le16(b, 36, touch_word(1000, 1));
    le16(b, 38, touch_word(3000, 2));
    le16(b, 40, touch_word(1002, 3));
    le16(b, 42, touch_word(3000, 5));
    const auto r = parse_input_report(b);
    REQUIRE(r);
    CHECK(r->touch.raw_x == 1009);
    CHECK(r->touch.raw_y == 3000);
    CHECK(r->touch.pressure == (1 | 2 << 3 | 3 << 6 | 5 << 9));
    CHECK(r->touch.pressed);
}

TEST_CASE("stick conversion: clamps, centers, inverts")
{
    CHECK(stick_to_axis(2050, 2050, false) == 0);
    CHECK(stick_to_axis(3200, 2050, false) == 32767);
    CHECK(stick_to_axis(900, 2050, false) == -32767);
    CHECK(stick_to_axis(4000, 2050, false) == 32767);  // beyond range: clamped
    CHECK(stick_to_axis(100, 2050, false) == -32767);
    CHECK(stick_to_axis(3200, 2050, true) == -32767);
}

TEST_CASE("stick conversion uses the learned center; both sides reach full scale")
{
    // Right stick Y on the user's GamePad rested at 1913.
    CHECK(stick_to_axis(1913, 1913, true) == 0);
    CHECK(stick_to_axis(3200, 1913, false) == 32767);
    CHECK(stick_to_axis(900, 1913, false) == -32767);
    // Halfway between center and max on the long side
    CHECK(stick_to_axis(1913 + (3200 - 1913) / 2, 1913, false) == doctest::Approx(16383).epsilon(0.01));
}

TEST_CASE("stick centers are learned only from resting sticks")
{
    InputReport r;
    r.sticks = {2034, 2180, 2106, 1913};  // values from the user's GamePad at rest
    const auto c = learn_stick_centers(r);
    REQUIRE(c);
    CHECK(c->center == r.sticks);
    r.sticks[2] = 3100;  // a stick is being pushed
    CHECK_FALSE(learn_stick_centers(r));
}

TEST_CASE("touch calibration maps the reference points exactly")
{
    const auto p1 = touch_to_screen(195, 3818);
    CHECK(p1.x == 20);
    CHECK(p1.y == 20);
    const auto p2 = touch_to_screen(3877, 373);
    CHECK(p2.x == 834);
    CHECK(p2.y == 460);
    const auto out = touch_to_screen(4095, 0);  // beyond the panel: clamped
    CHECK(out.x == 853);
    CHECK(out.y == 479);
}
