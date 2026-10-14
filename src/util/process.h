// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

namespace padlink {

// Runs a program (looked up in PATH) with arguments, without a shell, and
// waits for it. Returns its exit status (0 = success), or -1.
int run_command(const std::vector<std::string>& argv);

} // namespace padlink
