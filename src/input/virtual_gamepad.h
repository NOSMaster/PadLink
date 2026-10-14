// SPDX-License-Identifier: GPL-2.0-or-later
// Turns GamePad input reports into three Linux input devices:
//   "Wii U GamePad (padlink)"              buttons + sticks
//   "Wii U GamePad (padlink) Touchscreen"  854x480 absolute touch
//   "Wii U GamePad (padlink) Motion"       accelerometer + gyroscope (raw)
// This split matches how the kernel's own drivers (e.g. DualSense) expose
// touch and motion, so SDL and Steam recognize them.
#pragma once

#include "input/uinput_device.h"
#include "protocol/input.h"

#include <optional>
#include <string>
#include <vector>

namespace padlink {

// Pure mapping functions (unit tested).
std::vector<input_event> gamepad_events(const InputReport& r, const StickCalibration& sticks = {});
std::vector<input_event> touch_events(const InputReport& r, const TouchCalibration& cal = {});
std::vector<input_event> motion_events(const InputReport& r);
InputReport neutral_report(const StickCalibration& sticks = {});  // everything released and centered

class VirtualGamepad {
public:
    static std::optional<VirtualGamepad> create(std::string& error);

    void update(const InputReport& r);
    void release_all() { update(neutral_report(m_sticks)); }  // on signal loss
    void set_stick_calibration(const StickCalibration& c) { m_sticks = c; }

private:
    VirtualGamepad(UinputDevice pad, UinputDevice touch, UinputDevice motion)
        : m_pad(std::move(pad)), m_touch(std::move(touch)), m_motion(std::move(motion)) {}

    UinputDevice m_pad, m_touch, m_motion;
    StickCalibration m_sticks;
};

} // namespace padlink
