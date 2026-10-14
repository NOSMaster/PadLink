// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/drc_sockets.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>

namespace padlink {

namespace {
constexpr const char* kConsoleIp = "192.168.1.10";
constexpr const char* kGamepadIp = "192.168.1.11";
} // namespace

size_t DrcSockets::index(DrcPort port)
{
    for (size_t i = 0; i < kAll.size(); ++i)
        if (kAll[i] == port)
            return i;
    return 0;
}

bool DrcSockets::open(const std::string& iface, std::string& error)
{
    for (DrcPort port : kAll) {
        UniqueFd fd(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        if (!fd) {
            error = std::string("socket: ") + std::strerror(errno);
            return false;
        }
        const int one = 1;
        ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (::setsockopt(fd.get(), SOL_SOCKET, SO_BINDTODEVICE, iface.c_str(), socklen_t(iface.size() + 1)) != 0) {
            error = std::string("SO_BINDTODEVICE: ") + std::strerror(errno);
            return false;
        }
        if (port == DrcPort::Video || port == DrcPort::Audio) {
            // Media goes out as 802.11 QoS TID 5 ("video" access category),
            // like a real console.
            const int priority = 5, tos = 0xa0;
            ::setsockopt(fd.get(), SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority));
            ::setsockopt(fd.get(), IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(uint16_t(port));
        inet_pton(AF_INET, kConsoleIp, &addr.sin_addr);
        if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            error = "bind " + std::string(kConsoleIp) + ":" + std::to_string(uint16_t(port)) + ": " + std::strerror(errno);
            return false;
        }
        m_fds[index(port)] = std::move(fd);
    }
    return true;
}

bool DrcSockets::send(DrcPort port, ByteView data)
{
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(uint16_t(uint16_t(port) + 100));
    inet_pton(AF_INET, kGamepadIp, &to.sin_addr);
    return ::sendto(fd(port), data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) ==
           ssize_t(data.size());
}

ssize_t DrcSockets::receive(DrcPort port, uint8_t* buf, size_t size)
{
    sockaddr_in from{};
    socklen_t len = sizeof(from);
    const ssize_t n = ::recvfrom(fd(port), buf, size, 0, reinterpret_cast<sockaddr*>(&from), &len);
    in_addr pad{};
    inet_pton(AF_INET, kGamepadIp, &pad);
    if (n >= 0 && from.sin_addr.s_addr != pad.s_addr)
        return -1;  // only accept traffic from the GamePad
    return n;
}

} // namespace padlink
