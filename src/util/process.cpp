// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/process.h"

#include <csignal>
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

namespace padlink {

int run_command(const std::vector<std::string>& argv)
{
    if (argv.empty())
        return -1;
    std::vector<char*> args;
    for (const auto& a : argv)
        args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    // padlinkd blocks SIGINT/SIGTERM for its own signalfd; children must not
    // inherit that mask.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);

    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, args[0], nullptr, &attr, args.data(), environ);
    posix_spawnattr_destroy(&attr);
    if (rc != 0)
        return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

} // namespace padlink
