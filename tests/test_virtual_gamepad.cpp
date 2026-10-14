// SPDX-License-Identifier: GPL-2.0-or-later
#include "doctest.h"
#include "input/virtual_gamepad.h"

#include <map>

using namespace padlink;

namespace {
// (type, code) -> value, for easy lookups.
std::map<std::pair<int, int>, int> as_map(const std::vector<input_event>& ev)
{
    std::map<std::pair<int, int>, int> m;
    for (const auto& e : ev)
        m[{e.type, e.code}] = e.value;
    return m;
}
} // namespace

TEST_CASE("buttons map to Linux positional names (Nintendo layout)")
{
    InputReport r = neutral_report();
    r.buttons = btn::B | btn::A | btn::Home | btn::L3 | btn::ZR;
    const auto m = as_map(gamepad_events(r));
    CHECK(m.at({EV_KEY, BTN_SOUTH}) == 1);  // B is the bottom button
    CHECK(m.at({EV_KEY, BTN_EAST}) == 1);   // A is the right button
    CHECK(m.at({EV_KEY, BTN_NORTH}) == 0);
    CHECK(m.at({EV_KEY, BTN_MODE}) == 1);
    CHECK(m.at({EV_KEY, BTN_THUMBL}) == 1);
    CHECK(m.at({EV_KEY, BTN_TR2}) == 1);
    CHECK(m.at({EV_KEY, BTN_TL2}) == 0);
}

TEST_CASE("d-pad becomes a hat; sticks are centered and Y is inverted")
{
    InputReport r = neutral_report();
    r.buttons = btn::Left | btn::Down;
    r.sticks = {3200, 3200, 2050, 900};
    const auto m = as_map(gamepad_events(r));
    CHECK(m.at({EV_ABS, ABS_HAT0X}) == -1);
    CHECK(m.at({EV_ABS, ABS_HAT0Y}) == 1);
    CHECK(m.at({EV_ABS, ABS_X}) == 32767);
    CHECK(m.at({EV_ABS, ABS_Y}) == -32767);  // raw max = pushed up = negative
    CHECK(m.at({EV_ABS, ABS_RX}) == 0);
    CHECK(m.at({EV_ABS, ABS_RY}) == 32767);
}

TEST_CASE("neutral report releases everything")
{
    for (const auto& e : gamepad_events(neutral_report()))
        CHECK(e.value == 0);
    const auto t = as_map(touch_events(neutral_report()));
    CHECK(t.at({EV_KEY, BTN_TOUCH}) == 0);
    CHECK_FALSE(t.contains({EV_ABS, ABS_X}));  // no position without a touch
}

TEST_CASE("touch events carry calibrated pixel coordinates")
{
    InputReport r = neutral_report();
    r.touch = {true, 195, 3818, 100};
    const auto m = as_map(touch_events(r));
    CHECK(m.at({EV_KEY, BTN_TOUCH}) == 1);
    CHECK(m.at({EV_ABS, ABS_X}) == 20);
    CHECK(m.at({EV_ABS, ABS_Y}) == 20);
    CHECK(m.at({EV_ABS, ABS_PRESSURE}) == 100);
}

TEST_CASE("motion events pass raw sensor values through")
{
    InputReport r = neutral_report();
    r.accel = {1, -2, 3};
    r.gyro = {-4, 5, -6};
    const auto m = as_map(motion_events(r));
    CHECK(m.at({EV_ABS, ABS_Y}) == -2);
    CHECK(m.at({EV_ABS, ABS_RZ}) == -6);
}
