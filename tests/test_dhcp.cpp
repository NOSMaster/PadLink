// SPDX-License-Identifier: GPL-2.0-or-later
#include "doctest.h"
#include "protocol/dhcp.h"

#include <map>

using namespace padlink;

namespace {

Bytes make_request(DhcpType type, std::optional<uint32_t> requested = std::nullopt)
{
    Bytes p(236, 0);
    p[0] = 1; p[1] = 1; p[2] = 6;
    p[4] = 0xde; p[5] = 0xad; p[6] = 0xbe; p[7] = 0xef;  // xid
    p[10] = 0x80;                                          // broadcast flag
    const uint8_t mac[] = {0x40, 0xd2, 0x8a, 0xab, 0x90, 0x00};
    std::copy(std::begin(mac), std::end(mac), p.begin() + 28);
    put_be32(p, 0x63825363);
    p.insert(p.end(), {53, 1, uint8_t(type)});
    if (requested) {
        p.insert(p.end(), {50, 4});
        put_be32(p, *requested);
    }
    p.push_back(255);
    return p;
}

std::map<uint8_t, Bytes> options_of(const Bytes& reply)
{
    std::map<uint8_t, Bytes> out;
    for (size_t i = 240; i < reply.size();) {
        const uint8_t code = reply[i++];
        if (code == 0) continue;
        if (code == 255) break;
        const uint8_t len = reply[i++];
        out[code] = Bytes(reply.begin() + i, reply.begin() + i + len);
        i += len;
    }
    return out;
}

} // namespace

TEST_CASE("DISCOVER is parsed")
{
    const auto req = parse_dhcp_request(make_request(DhcpType::Discover));
    REQUIRE(req);
    CHECK(req->type == DhcpType::Discover);
    CHECK(req->xid == 0xdeadbeef);
    CHECK(req->flags == 0x8000);
    CHECK(req->client_mac.to_string() == "40:d2:8a:ab:90:00");
    CHECK_FALSE(req->requested_ip);
}

TEST_CASE("requested address option is parsed")
{
    const auto req = parse_dhcp_request(make_request(DhcpType::Request, 0xc0a8010b));
    REQUIRE(req);
    CHECK(req->type == DhcpType::Request);
    CHECK(req->requested_ip == 0xc0a8010bu);
}

TEST_CASE("non-requests and truncated packets are rejected")
{
    Bytes reply = make_request(DhcpType::Discover);
    reply[0] = 2;  // BOOTREPLY
    CHECK_FALSE(parse_dhcp_request(reply));
    CHECK_FALSE(parse_dhcp_request(Bytes(100, 0)));
    Bytes bad_option = make_request(DhcpType::Discover);
    bad_option.back() = 53;  // option code without length: truncated
    CHECK_FALSE(parse_dhcp_request(bad_option));
}

TEST_CASE("ACK carries the fixed GamePad lease and MTU 1800")
{
    const auto req = *parse_dhcp_request(make_request(DhcpType::Request, 0xc0a8010b));
    const Bytes ack = build_dhcp_reply(req, DhcpType::Ack, DhcpLease{});
    REQUIRE(ack.size() >= 300);
    CHECK(ack[0] == 2);
    CHECK(read_be32(&ack[4]) == 0xdeadbeef);
    CHECK(read_be32(&ack[16]) == 0xc0a8010b);  // yiaddr 192.168.1.11
    CHECK(read_be32(&ack[236]) == 0x63825363);
    const auto opts = options_of(ack);
    CHECK(opts.at(53) == Bytes{5});
    CHECK(read_be32(opts.at(54).data()) == 0xc0a8010a);
    CHECK(read_be32(opts.at(1).data()) == 0xffffff00);
    CHECK(read_be32(opts.at(3).data()) == 0xc0a8010a);
    CHECK(read_be32(opts.at(28).data()) == 0xc0a801ff);
    CHECK(read_be32(opts.at(51).data()) == 3600);
    CHECK(read_be16(opts.at(26).data()) == 1800);
}

TEST_CASE("NAK has no address")
{
    const auto req = *parse_dhcp_request(make_request(DhcpType::Request, 0x0a000002));
    const Bytes nak = build_dhcp_reply(req, DhcpType::Nak, DhcpLease{});
    CHECK(read_be32(&nak[16]) == 0);
    const auto opts = options_of(nak);
    CHECK(opts.at(53) == Bytes{6});
    CHECK_FALSE(opts.contains(51));
}
