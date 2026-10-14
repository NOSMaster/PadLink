// SPDX-License-Identifier: GPL-2.0-or-later
#include "protocol/dhcp.h"

#include <algorithm>

namespace padlink {

namespace {

constexpr size_t kFixedHeader = 236;        // op .. file fields
constexpr uint32_t kMagicCookie = 0x63825363;
constexpr uint8_t kBootRequest = 1;
constexpr uint8_t kBootReply = 2;

namespace opt {
constexpr uint8_t Pad = 0, SubnetMask = 1, Router = 3, Mtu = 26, Broadcast = 28,
                  RequestedIp = 50, LeaseTime = 51, MessageType = 53, ServerId = 54, End = 255;
}

void put_option(Bytes& out, uint8_t code, ByteView value)
{
    out.push_back(code);
    out.push_back(uint8_t(value.size()));
    put_bytes(out, value);
}

void put_option_u32(Bytes& out, uint8_t code, uint32_t value)
{
    Bytes v;
    put_be32(v, value);
    put_option(out, code, v);
}

} // namespace

std::optional<DhcpRequest> parse_dhcp_request(ByteView p)
{
    if (p.size() < kFixedHeader + 4 || p[0] != kBootRequest || p[1] != 1 /* Ethernet */ || p[2] != 6)
        return std::nullopt;
    if (read_be32(&p[kFixedHeader]) != kMagicCookie)
        return std::nullopt;

    DhcpRequest req;
    req.xid = read_be32(&p[4]);
    req.flags = read_be16(&p[10]);
    std::copy_n(&p[28], 6, req.client_mac.bytes.begin());

    bool have_type = false;
    // Options are type-length-value records after the magic cookie.
    for (size_t i = kFixedHeader + 4; i < p.size();) {
        const uint8_t code = p[i++];
        if (code == opt::Pad) continue;
        if (code == opt::End) break;
        if (i >= p.size()) return std::nullopt;
        const uint8_t len = p[i++];
        if (i + len > p.size()) return std::nullopt;
        if (code == opt::MessageType && len == 1) {
            req.type = DhcpType(p[i]);
            have_type = true;
        } else if (code == opt::RequestedIp && len == 4) {
            req.requested_ip = read_be32(&p[i]);
        }
        i += len;
    }
    if (!have_type)
        return std::nullopt;
    return req;
}

Bytes build_dhcp_reply(const DhcpRequest& req, DhcpType reply_type, const DhcpLease& lease)
{
    Bytes out(kFixedHeader, 0);
    out[0] = kBootReply;
    out[1] = 1;
    out[2] = 6;
    auto put32_at = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out[at + i] = uint8_t(v >> (24 - 8 * i));
    };
    put32_at(4, req.xid);
    out[10] = uint8_t(req.flags >> 8);
    out[11] = uint8_t(req.flags);
    if (reply_type != DhcpType::Nak) {
        put32_at(16, lease.client_ip);   // yiaddr: "your" address
        put32_at(20, lease.server_ip);   // siaddr
    }
    std::copy(req.client_mac.bytes.begin(), req.client_mac.bytes.end(), out.begin() + 28);

    put_be32(out, kMagicCookie);
    const uint8_t type[] = {uint8_t(reply_type)};
    put_option(out, opt::MessageType, type);
    put_option_u32(out, opt::ServerId, lease.server_ip);
    if (reply_type != DhcpType::Nak) {
        put_option_u32(out, opt::LeaseTime, lease.lease_seconds);
        put_option_u32(out, opt::SubnetMask, lease.netmask);
        put_option_u32(out, opt::Router, lease.server_ip);
        put_option_u32(out, opt::Broadcast, lease.broadcast);
        Bytes mtu;
        put_be16(mtu, lease.mtu);
        put_option(out, opt::Mtu, mtu);
    }
    out.push_back(opt::End);
    // Some clients reject replies shorter than the classic 300-byte BOOTP size.
    if (out.size() < 300)
        out.resize(300, 0);
    return out;
}

} // namespace padlink
