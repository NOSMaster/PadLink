// SPDX-License-Identifier: GPL-2.0-or-later
// Tiny DHCP server: gives every client on the GamePad interface 192.168.1.11.
#pragma once

#include "protocol/dhcp.h"
#include "util/unique_fd.h"

#include <optional>
#include <string>

namespace padlink {

class DhcpServer {
public:
    // Binds UDP port 67 on `iface` only (needs root/CAP_NET_RAW).
    bool open(const std::string& iface, std::string& error);
    int fd() const { return m_fd.get(); }

    // Handles one pending request. Returns the client MAC when a lease was
    // acknowledged (DHCPACK sent).
    std::optional<MacAddress> handle_one();

private:
    UniqueFd m_fd;
    DhcpLease m_lease;
};

} // namespace padlink
