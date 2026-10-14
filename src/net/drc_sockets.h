// SPDX-License-Identifier: GPL-2.0-or-later
// The console-side UDP sockets (PROTOCOL.md §2), bound to 192.168.1.10 on the
// GamePad interface. Pad port = console port + 100.
#pragma once

#include "protocol/bytes.h"
#include "util/unique_fd.h"

#include <array>
#include <string>

namespace padlink {

enum class DrcPort : uint16_t {
    Msg = 50010, Video = 50020, Audio = 50021, Input = 50022, Cmd = 50023, Unknown = 50025,
};

class DrcSockets {
public:
    static constexpr std::array<DrcPort, 6> kAll{DrcPort::Msg, DrcPort::Video, DrcPort::Audio,
                                                 DrcPort::Input, DrcPort::Cmd, DrcPort::Unknown};

    bool open(const std::string& iface, std::string& error);
    int fd(DrcPort port) const { return m_fds[index(port)].get(); }

    // Sends to the GamePad (192.168.1.11) on the matching pad port.
    bool send(DrcPort port, ByteView data);

    // Receives one datagram from the GamePad; returns its size, or -1.
    ssize_t receive(DrcPort port, uint8_t* buf, size_t size);

private:
    static size_t index(DrcPort port);
    std::array<UniqueFd, 6> m_fds;
};

} // namespace padlink
