// SPDX-License-Identifier: GPL-2.0-or-later
#include "input/virtual_gamepad.h"

#include <array>
#include <utility>

namespace padlink {

namespace {

// Nintendo layout: B is the bottom face button, A the right one. Linux names
// buttons by position (BTN_SOUTH = bottom), like the kernel's Nintendo driver.
constexpr std::array<std::pair<uint32_t, uint16_t>, 14> kButtonMap{{
    {btn::B, BTN_SOUTH}, {btn::A, BTN_EAST}, {btn::X, BTN_NORTH}, {btn::Y, BTN_WEST},
    {btn::L, BTN_TL}, {btn::R, BTN_TR}, {btn::ZL, BTN_TL2}, {btn::ZR, BTN_TR2},
    {btn::Minus, BTN_SELECT}, {btn::Plus, BTN_START}, {btn::Home, BTN_MODE},
    {btn::L3, BTN_THUMBL}, {btn::R3, BTN_THUMBR}, {btn::TV, BTN_TRIGGER_HAPPY1},
}};

int hat(bool negative, bool positive) { return positive - negative; }

} // namespace

InputReport neutral_report(const StickCalibration& sticks)
{
    InputReport r;
    r.sticks = sticks.center;
    return r;
}

std::vector<input_event> gamepad_events(const InputReport& r, const StickCalibration& c)
{
    std::vector<input_event> ev;
    for (const auto& [mask, code] : kButtonMap)
        ev.push_back(make_event(EV_KEY, code, (r.buttons & mask) != 0));
    ev.push_back(make_event(EV_ABS, ABS_HAT0X, hat(r.buttons & btn::Left, r.buttons & btn::Right)));
    ev.push_back(make_event(EV_ABS, ABS_HAT0Y, hat(r.buttons & btn::Up, r.buttons & btn::Down)));
    // Linux convention: pushing a stick up/left gives negative values. The
    // GamePad's raw Y grows when pushed up, so Y is inverted.
    ev.push_back(make_event(EV_ABS, ABS_X, stick_to_axis(r.sticks[0], c.center[0], false)));
    ev.push_back(make_event(EV_ABS, ABS_Y, stick_to_axis(r.sticks[1], c.center[1], true)));
    ev.push_back(make_event(EV_ABS, ABS_RX, stick_to_axis(r.sticks[2], c.center[2], false)));
    ev.push_back(make_event(EV_ABS, ABS_RY, stick_to_axis(r.sticks[3], c.center[3], true)));
    return ev;
}

std::vector<input_event> touch_events(const InputReport& r, const TouchCalibration& cal)
{
    std::vector<input_event> ev;
    ev.push_back(make_event(EV_KEY, BTN_TOUCH, r.touch.pressed));
    if (r.touch.pressed) {
        const ScreenPoint p = touch_to_screen(r.touch.raw_x, r.touch.raw_y, cal);
        ev.push_back(make_event(EV_ABS, ABS_X, p.x));
        ev.push_back(make_event(EV_ABS, ABS_Y, p.y));
    }
    ev.push_back(make_event(EV_ABS, ABS_PRESSURE, r.touch.pressure));
    return ev;
}

std::vector<input_event> motion_events(const InputReport& r)
{
    // Raw sensor values; axis order and scale are not known yet (PROTOCOL.md Q3).
    return {
        make_event(EV_ABS, ABS_X, r.accel[0]), make_event(EV_ABS, ABS_Y, r.accel[1]),
        make_event(EV_ABS, ABS_Z, r.accel[2]), make_event(EV_ABS, ABS_RX, r.gyro[0]),
        make_event(EV_ABS, ABS_RY, r.gyro[1]), make_event(EV_ABS, ABS_RZ, r.gyro[2]),
    };
}

std::optional<VirtualGamepad> VirtualGamepad::create(std::string& error)
{
    UinputSpec pad;
    pad.name = "Wii U GamePad (padlink)";
    for (const auto& [mask, code] : kButtonMap)
        pad.keys.push_back(code);
    pad.axes = {
        // flat = dead zone reported to games (~10%): resting sticks wander
        // by about ±50 raw units even after centering.
        {ABS_X, -32767, 32767, 128, 3300}, {ABS_Y, -32767, 32767, 128, 3300},
        {ABS_RX, -32767, 32767, 128, 3300}, {ABS_RY, -32767, 32767, 128, 3300},
        {ABS_HAT0X, -1, 1}, {ABS_HAT0Y, -1, 1},
    };

    UinputSpec touch;
    touch.name = "Wii U GamePad (padlink) Touchscreen";
    touch.keys = {BTN_TOUCH};
    // ~6 units per mm on the 6.2" 854x480 panel.
    touch.axes = {{ABS_X, 0, 853, 0, 0, 6}, {ABS_Y, 0, 479, 0, 0, 6}, {ABS_PRESSURE, 0, 4095}};
    touch.properties = {INPUT_PROP_DIRECT};

    UinputSpec motion;
    motion.name = "Wii U GamePad (padlink) Motion";
    motion.axes = {
        {ABS_X, -32768, 32767, 16}, {ABS_Y, -32768, 32767, 16}, {ABS_Z, -32768, 32767, 16},
        {ABS_RX, -8388608, 8388607, 64}, {ABS_RY, -8388608, 8388607, 64}, {ABS_RZ, -8388608, 8388607, 64},
    };
    motion.properties = {INPUT_PROP_ACCELEROMETER};

    auto p = UinputDevice::create(pad, error);
    if (!p) return std::nullopt;
    auto t = UinputDevice::create(touch, error);
    if (!t) return std::nullopt;
    auto m = UinputDevice::create(motion, error);
    if (!m) return std::nullopt;
    return VirtualGamepad(std::move(*p), std::move(*t), std::move(*m));
}

void VirtualGamepad::update(const InputReport& r)
{
    m_pad.emit(gamepad_events(r, m_sticks));
    m_touch.emit(touch_events(r));
    m_motion.emit(motion_events(r));
}

} // namespace padlink
