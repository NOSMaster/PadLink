// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal timestamped logging to stderr.
#pragma once

#include <chrono>
#include <cstdio>
#include <format>
#include <string_view>

namespace padlink {

// Seconds since the program started. Kept outside the template below: a
// `static` inside a template exists once per instantiation (i.e. per distinct
// set of argument types), which would give every message type its own clock.
inline double log_uptime()
{
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

template <typename... Args>
void log(std::format_string<Args...> fmt, Args&&... args)
{
    std::fprintf(stderr, "[%8.3f] %s\n", log_uptime(), std::format(fmt, std::forward<Args>(args)...).c_str());
}

} // namespace padlink
