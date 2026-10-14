// SPDX-License-Identifier: GPL-2.0-or-later
// RAII owner for a Linux file descriptor (socket, file, ...). Move-only, so
// a descriptor is closed exactly once.
#pragma once

#include <unistd.h>
#include <utility>

namespace padlink {

class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : m_fd(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other)
            reset(std::exchange(other.m_fd, -1));
        return *this;
    }

    int get() const { return m_fd; }
    explicit operator bool() const { return m_fd >= 0; }

    void reset(int fd = -1)
    {
        if (m_fd >= 0)
            ::close(m_fd);
        m_fd = fd;
    }

private:
    int m_fd = -1;
};

} // namespace padlink
