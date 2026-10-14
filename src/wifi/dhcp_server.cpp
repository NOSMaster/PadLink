// SPDX-License-Identifier: GPL-2.0-or-later
#include "wifi/dhcp_server.h"

#include "util/log.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>

namespace padlink {

bool DhcpServer::open(const std::string& iface, std::string& error)
{
    m_fd.reset(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!m_fd) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    const int one = 1;
    ::setsockopt(m_fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(m_fd.get(), SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    // Only listen on the GamePad interface, never on other networks.
    if (::setsockopt(m_fd.get(), SOL_SOCKET, SO_BINDTODEVICE, iface.c_str(), socklen_t(iface.size() + 1)) != 0) {
        error = std::string("SO_BINDTODEVICE: ") + std::strerror(errno);
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(67);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(m_fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = std::string("bind udp/67: ") + std::strerror(errno);
        return false;
    }
    return true;
}

std::optional<MacAddress> DhcpServer::handle_one()
{
    uint8_t buf[1500];
    const ssize_t n = ::recv(m_fd.get(), buf, sizeof(buf), 0);
    if (n <= 0)
        return std::nullopt;
    const auto req = parse_dhcp_request(ByteView(buf, size_t(n)));
    if (!req)
        return std::nullopt;

    DhcpType reply;
    switch (req->type) {
    case DhcpType::Discover:
        reply = DhcpType::Offer;
        break;
    case DhcpType::Request:
        // Refuse a different address the client remembers from elsewhere,
        // so it falls back to DISCOVER and gets ours.
        reply = (req->requested_ip && *req->requested_ip != m_lease.client_ip) ? DhcpType::Nak : DhcpType::Ack;
        break;
    default:
        log("dhcp: ignoring message type {} from {}", int(req->type), req->client_mac.to_string());
        return std::nullopt;
    }

    const Bytes out = build_dhcp_reply(*req, reply, m_lease);
    // The client has no address yet, so reply by broadcast on this interface.
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(68);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    if (::sendto(m_fd.get(), out.data(), out.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) < 0) {
        log("dhcp: send failed: {}", std::strerror(errno));
        return std::nullopt;
    }
    const char* name = reply == DhcpType::Offer ? "OFFER" : reply == DhcpType::Ack ? "ACK" : "NAK";
    log("dhcp: {} 192.168.1.11 -> {}", name, req->client_mac.to_string());
    if (reply == DhcpType::Ack)
        return req->client_mac;
    return std::nullopt;
}

} // namespace padlink
