// SPDX-License-Identifier: GPL-2.0-or-later
#include "protocol/input.h"

#include <algorithm>

namespace padlink {

namespace {

int32_t read_le_s24(const uint8_t* p)
{
    int32_t v = p[0] | p[1] << 8 | p[2] << 16;
    if (v & 0x800000)  // sign-extend the 24-bit value to 32 bits
        v -= 0x1000000;
    return v;
}

constexpr int kStickMin = 900, kStickMax = 3200;

} // namespace

std::optional<InputReport> parse_input_report(ByteView p)
{
    if (p.size() != kInputReportSize)
        return std::nullopt;

    InputReport r;
    r.sequence = read_be16(&p[0]);
    r.buttons = uint32_t(p[80]) << 16 | read_be16(&p[2]);
    r.power_status = p[4];
    r.battery = p[5];
    for (size_t i = 0; i < 4; ++i)
        r.sticks[i] = read_le16(&p[6 + 2 * i]);
    r.volume = p[14];
    for (size_t i = 0; i < 3; ++i)
        r.accel[i] = int16_t(read_le16(&p[15 + 2 * i]));
    for (size_t i = 0; i < 3; ++i)
        r.gyro[i] = read_le_s24(&p[21 + 3 * i]);
    std::copy_n(&p[30], 6, r.magnet.begin());

    // 10 touch samples of (x, y), each a LE u16: low 12 bits = coordinate,
    // bits 12..14 = 3 "extra" bits.
    uint32_t sum_x = 0, sum_y = 0;
    for (size_t i = 0; i < 10; ++i) {
        sum_x += read_le16(&p[36 + 4 * i]) & 0x0fff;
        sum_y += read_le16(&p[38 + 4 * i]) & 0x0fff;
    }
    // Pressure: the extra bits of point0.x, point0.y, point1.x, point1.y.
    uint16_t pressure = 0;
    for (size_t i = 0; i < 4; ++i)
        pressure |= uint16_t(((p[37 + 2 * i] >> 4) & 0x07) << (3 * i));
    r.touch.raw_x = uint16_t(sum_x / 10);
    r.touch.raw_y = uint16_t(sum_y / 10);
    r.touch.pressure = pressure;
    r.touch.pressed = pressure != 0;

    r.waiting_for_video = (p[83] & 0x01) != 0;
    r.firmware_version = uint8_t(~p[127]);
    return r;
}

std::optional<StickCalibration> learn_stick_centers(const InputReport& r)
{
    StickCalibration cal;
    for (size_t i = 0; i < 4; ++i) {
        if (r.sticks[i] < 1650 || r.sticks[i] > 2450)
            return std::nullopt;
        cal.center[i] = r.sticks[i];
    }
    return cal;
}

int16_t stick_to_axis(uint16_t raw, uint16_t center, bool invert)
{
    const int c = std::clamp(int(center), kStickMin + 1, kStickMax - 1);
    const int v = std::clamp(int(raw), kStickMin, kStickMax) - c;
    int out = v >= 0 ? v * 32767 / (kStickMax - c) : v * 32767 / (c - kStickMin);
    if (invert)
        out = -out;
    return int16_t(std::clamp(out, -32767, 32767));
}

ScreenPoint touch_to_screen(uint16_t raw_x, uint16_t raw_y, const TouchCalibration& c)
{
    // Straight line through the two calibration points, per axis.
    const int x = c.ref1_x + (int(raw_x) - c.raw1_x) * (c.ref2_x - c.ref1_x) / (c.raw2_x - c.raw1_x);
    const int y = c.ref1_y + (int(raw_y) - c.raw1_y) * (c.ref2_y - c.ref1_y) / (c.raw2_y - c.raw1_y);
    return {std::clamp(x, 0, 853), std::clamp(y, 0, 479)};
}

} // namespace padlink
