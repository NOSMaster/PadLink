// SPDX-License-Identifier: GPL-2.0-or-later
// Just enough DHCP (RFC 2131) to hand the GamePad its fixed address.
#pragma once

#include "protocol/bytes.h"
#include "protocol/pairing.h"

#include <optional>

namespace padlink {

enum class DhcpType : uint8_t { Discover = 1, Offer = 2, Request = 3, Decline = 4, Ack = 5, Nak = 6, Release = 7 };

struct DhcpRequest {
    DhcpType type{};
    uint32_t xid = 0;           // transaction id, copied into the reply
    uint16_t flags = 0;         // bit 15 = client wants a broadcast reply
    MacAddress client_mac;
    std::optional<uint32_t> requested_ip;  // option 50, host byte order
};

// IPv4 addresses below are in host byte order (192.168.1.10 = 0xc0a8010a).
struct DhcpLease {
    uint32_t server_ip = 0xc0a8010a;   // 192.168.1.10 (the "console")
    uint32_t client_ip = 0xc0a8010b;   // 192.168.1.11 (GamePad 1)
    uint32_t netmask = 0xffffff00;
    uint32_t broadcast = 0xc0a801ff;
    uint32_t lease_seconds = 3600;
    uint16_t mtu = 1800;
};

// Parses a client (BOOTREQUEST) message; std::nullopt if it isn't one.
std::optional<DhcpRequest> parse_dhcp_request(ByteView packet);

// Builds an OFFER/ACK/NAK reply for `request`.
Bytes build_dhcp_reply(const DhcpRequest& request, DhcpType reply_type, const DhcpLease& lease);

} // namespace padlink
