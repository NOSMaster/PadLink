// SPDX-License-Identifier: GPL-2.0-or-later
// The 128-byte HID input report the GamePad sends ~180 times per second to
// UDP port 50022. Layout: PROTOCOL.md §3.
#pragma once

#include "protocol/bytes.h"

#include <array>
#include <optional>

namespace padlink {

inline constexpr size_t kInputReportSize = 128;

// 24-bit combined mask: (byte 80 << 16) | big-endian bytes 2..3.
namespace btn {
inline constexpr uint32_t Sync = 0x000001, Home = 0x000002, Minus = 0x000004, Plus = 0x000008,
                          R = 0x000010, L = 0x000020, ZR = 0x000040, ZL = 0x000080,
                          Down = 0x000100, Up = 0x000200, Right = 0x000400, Left = 0x000800,
                          Y = 0x001000, X = 0x002000, B = 0x004000, A = 0x008000,
                          TV = 0x200000, R3 = 0x400000, L3 = 0x800000;
}

// Power status bits (byte 4).
namespace power {
inline constexpr uint8_t AcPlugged = 0x01, PowerButton = 0x02, Charging = 0x40, Usb = 0x80;
}

struct TouchSample {
    bool pressed = false;
    uint16_t raw_x = 0, raw_y = 0;  // average of the 10 samples, 12-bit
    uint16_t pressure = 0;          // 12-bit, units unknown (resistance?)
};

struct InputReport {
    uint16_t sequence = 0;
    uint32_t buttons = 0;
    uint8_t power_status = 0;
    uint8_t battery = 0;
    uint8_t volume = 0;
    std::array<uint16_t, 4> sticks{};    // raw: left X, left Y, right X, right Y
    std::array<int16_t, 3> accel{};      // raw, order as in the packet
    std::array<int32_t, 3> gyro{};       // raw signed 24-bit, order as in the packet
    std::array<uint8_t, 6> magnet{};     // format unknown
    TouchSample touch;
    bool waiting_for_video = false;      // byte 83 bit 0 [observed]
    uint8_t firmware_version = 0;        // ~byte 127
};

std::optional<InputReport> parse_input_report(ByteView packet);

// --- conversions used by the uinput layer (pure, unit tested) ---

// Resting ("center") raw value of each stick axis. Real sticks are not
// exactly at 2050; padlink learns the centers when the GamePad connects.
struct StickCalibration {
    std::array<uint16_t, 4> center{2050, 2050, 2050, 2050};
};

// Takes the current stick positions as centers, if they all look like a
// stick at rest (within ±400 of the nominal center). Otherwise std::nullopt
// (someone is holding a stick), and the caller should try a later report.
std::optional<StickCalibration> learn_stick_centers(const InputReport& r);

// Raw stick value (about 900..3200) -> -32767..32767. Each side of `center`
// is scaled separately, so full deflection still reaches ±32767.
// `invert` flips the direction (Linux: up/left is negative).
int16_t stick_to_axis(uint16_t raw, uint16_t center, bool invert);

// Touch calibration: two reference points mapping raw -> screen pixels.
struct TouchCalibration {
    int raw1_x = 195, raw1_y = 3818, ref1_x = 20, ref1_y = 20;
    int raw2_x = 3877, raw2_y = 373, ref2_x = 834, ref2_y = 460;
};
struct ScreenPoint {
    int x, y;  // 0..853, 0..479
};
ScreenPoint touch_to_screen(uint16_t raw_x, uint16_t raw_y, const TouchCalibration& cal = {});

} // namespace padlink
