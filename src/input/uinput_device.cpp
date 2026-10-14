// SPDX-License-Identifier: GPL-2.0-or-later
#include "input/uinput_device.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace padlink {

std::optional<UinputDevice> UinputDevice::create(const UinputSpec& spec, std::string& error)
{
    UinputDevice dev;
    dev.m_fd.reset(::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC));
    if (!dev.m_fd) {
        error = std::string("/dev/uinput: ") + std::strerror(errno);
        return std::nullopt;
    }
    const int fd = dev.m_fd.get();
    // Declare the device's capabilities.
    auto fail = [&](const char* what) {
        error = std::string(what) + ": " + std::strerror(errno);
        return std::nullopt;
    };
    if (!spec.keys.empty()) {
        if (::ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0)
            return fail("UI_SET_EVBIT EV_KEY");
        for (uint16_t k : spec.keys)
            if (::ioctl(fd, UI_SET_KEYBIT, k) < 0)
                return fail("UI_SET_KEYBIT");
    }
    if (!spec.axes.empty() && ::ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0)
        return fail("UI_SET_EVBIT EV_ABS");
    for (const auto& a : spec.axes) {
        uinput_abs_setup abs{};
        abs.code = a.code;
        abs.absinfo.minimum = a.min;
        abs.absinfo.maximum = a.max;
        abs.absinfo.fuzz = a.fuzz;
        abs.absinfo.flat = a.flat;
        abs.absinfo.resolution = a.resolution;
        if (::ioctl(fd, UI_SET_ABSBIT, a.code) < 0 || ::ioctl(fd, UI_ABS_SETUP, &abs) < 0)
            return fail("UI_ABS_SETUP");
    }
    for (int p : spec.properties)
        if (::ioctl(fd, UI_SET_PROPBIT, p) < 0)
            return fail("UI_SET_PROPBIT");

    uinput_setup setup{};
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = spec.vendor;
    setup.id.product = spec.product;
    setup.id.version = spec.version;
    std::strncpy(setup.name, spec.name.c_str(), UINPUT_MAX_NAME_SIZE - 1);
    if (::ioctl(fd, UI_DEV_SETUP, &setup) < 0)
        return fail("UI_DEV_SETUP");
    if (::ioctl(fd, UI_DEV_CREATE) < 0)
        return fail("UI_DEV_CREATE");
    return dev;
}

UinputDevice::~UinputDevice()
{
    if (m_fd)
        ::ioctl(m_fd.get(), UI_DEV_DESTROY);
}

bool UinputDevice::emit(const std::vector<input_event>& events)
{
    std::vector<input_event> batch = events;
    batch.push_back(make_event(EV_SYN, SYN_REPORT, 0));
    const size_t bytes = batch.size() * sizeof(input_event);
    return ::write(m_fd.get(), batch.data(), bytes) == ssize_t(bytes);
}

} // namespace padlink
