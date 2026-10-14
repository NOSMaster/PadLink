// SPDX-License-Identifier: GPL-2.0-or-later
// A virtual Linux input device created through /dev/uinput. Applications
// (games, SDL, Steam, evtest) see it like a real USB controller.
#pragma once

#include "util/unique_fd.h"

#include <cstdint>
#include <linux/input.h>
#include <optional>
#include <string>
#include <vector>

namespace padlink {

struct AbsAxis {
    uint16_t code;
    int min, max, fuzz = 0, flat = 0, resolution = 0;
};

struct UinputSpec {
    std::string name;
    uint16_t vendor = 0x1209, product = 0x5057, version = 1;  // see ARCHITECTURE.md
    std::vector<uint16_t> keys;
    std::vector<AbsAxis> axes;
    std::vector<int> properties;  // INPUT_PROP_*
};

class UinputDevice {
public:
    static std::optional<UinputDevice> create(const UinputSpec& spec, std::string& error);

    UinputDevice(UinputDevice&&) = default;
    ~UinputDevice();

    // Writes a batch of events followed by a SYN_REPORT ("this is one frame").
    // The kernel drops values that did not change, so sending the full state
    // every time is fine.
    bool emit(const std::vector<input_event>& events);

private:
    UinputDevice() = default;
    UniqueFd m_fd;
};

inline input_event make_event(uint16_t type, uint16_t code, int32_t value)
{
    input_event e{};
    e.type = type;
    e.code = code;
    e.value = value;
    return e;
}

} // namespace padlink
