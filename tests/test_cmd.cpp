// SPDX-License-Identifier: GPL-2.0-or-later
#include "doctest.h"
#include "protocol/cmd.h"

using namespace padlink;

TEST_CASE("CMD header is four little-endian u16 values")
{
    const uint8_t payload[] = {0xaa, 0xbb};
    const Bytes b = build_cmd(CmdType::Reply, CmdQuery::UvcUac, 0x1234, payload);
    CHECK(b == Bytes{0x02, 0x00, 0x01, 0x00, 0x02, 0x00, 0x34, 0x12, 0xaa, 0xbb});
    const auto p = parse_cmd(b);
    REQUIRE(p);
    CHECK(p->type == CmdType::Reply);
    CHECK(p->query == CmdQuery::UvcUac);
    CHECK(p->sequence == 0x1234);
    CHECK(p->payload == Bytes{0xaa, 0xbb});
}

TEST_CASE("malformed CMD packets are rejected")
{
    CHECK_FALSE(parse_cmd(Bytes{0, 0, 0}));
    CHECK_FALSE(parse_cmd(Bytes{4, 0, 0, 0, 0, 0, 0, 0}));     // bad type
    CHECK_FALSE(parse_cmd(Bytes{0, 0, 3, 0, 0, 0, 0, 0}));     // bad query type
    CHECK_FALSE(parse_cmd(Bytes{0, 0, 0, 0, 5, 0, 0, 0, 1}));  // length mismatch
}

TEST_CASE("UIC config query matches the real console bytes")
{
    CHECK(build_generic_query(kServicePeripheral, kMethodReadUicConfig) ==
          Bytes{0x7e, 0x01, 0x00, 0x08, 0x00, 0x40, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00});
    const uint8_t level[] = {3};
    CHECK(build_generic_query(kServicePeripheral, kMethodSetBrightness, level) ==
          Bytes{0x7e, 0x01, 0x00, 0x08, 0x00, 0x40, 0x05, 0x14, 0x00, 0x00, 0x00, 0x01, 0x03});
}

TEST_CASE("generic reply is parsed")
{
    Bytes body{0x7e, 0x01, 0x00, 0x08, 0x00, 0x01, 0x05, 0x06, 0x00, 0x00, 0x00, 0x02, 0xde, 0xad};
    const auto r = parse_generic_reply(body);
    REQUIRE(r);
    CHECK(r->service == 5);
    CHECK(r->method == 6);
    CHECK(r->error == 0);
    CHECK(r->data == Bytes{0xde, 0xad});
    body[11] = 3;  // declared length too long
    CHECK_FALSE(parse_generic_reply(body));
}

TEST_CASE("UVC/UAC query and synchronization")
{
    Bytes q = build_uvc_uac_query();
    REQUIRE(q.size() == 48);
    CHECK(read_le16(&q[12]) == 16000);
    CHECK(q[0] == 0);
    Bytes reply(16, 0);
    reply[0] = 0x34; reply[1] = 0x12;  // mic volume
    reply[12] = 1; reply[13] = 7; reply[14] = 2;
    sync_uvc_uac_query(q, reply);
    CHECK(q[0] == 1);
    CHECK(q[4] == 1);
    CHECK(read_le16(&q[6]) == 0x1234);
    CHECK(q[16] == 7);
    CHECK(q[17] == 1);
}

namespace {
struct Wire {
    std::vector<Bytes> sent;
    CommandChannel channel{[this](const Bytes& b) { sent.push_back(b); }};
    CmdPacket last() const { return *parse_cmd(sent.back()); }
};
} // namespace

TEST_CASE("query, ACK, reply, reply-ACK; one-shot transaction finishes")
{
    Wire w;
    int replies = 0;
    w.channel.add(CmdQuery::Generic, Bytes{1, 2}, 0, 0, [&](const Bytes& p) {
        ++replies;
        CHECK(p == Bytes{9});
    });
    w.channel.poll(0);
    REQUIRE(w.sent.size() == 1);
    const CmdPacket q = w.last();
    CHECK(q.type == CmdType::Query);
    CHECK(q.payload == Bytes{1, 2});

    w.channel.on_packet(build_cmd(CmdType::QueryAck, CmdQuery::Generic, q.sequence), 1000);
    w.channel.poll(200000);  // ACKed: no resend while waiting for the reply
    CHECK(w.sent.size() == 1);

    const uint8_t data[] = {9};
    w.channel.on_packet(build_cmd(CmdType::Reply, CmdQuery::Generic, q.sequence, data), 300000);
    REQUIRE(w.sent.size() == 2);
    CHECK(w.last().type == CmdType::ReplyAck);
    CHECK(w.last().sequence == q.sequence);
    CHECK(replies == 1);

    w.channel.poll(10000000);
    CHECK(w.sent.size() == 2);  // done for good
}

TEST_CASE("unacknowledged queries are retried, then backed off")
{
    Wire w;
    w.channel.add(CmdQuery::UvcUac, Bytes{}, 0, 1000000, nullptr);
    int64_t t = 0;
    for (int i = 0; i < CommandChannel::kMaxAttempts; ++i, t += CommandChannel::kRetryUs)
        w.channel.poll(t);
    CHECK(w.sent.size() == size_t(CommandChannel::kMaxAttempts));
    CHECK(w.channel.stats().retries == uint64_t(CommandChannel::kMaxAttempts - 1));
    // all attempts share one sequence number
    CHECK(parse_cmd(w.sent.front())->sequence == parse_cmd(w.sent.back())->sequence);

    w.channel.poll(t);  // gives up
    CHECK(w.channel.stats().timeouts == 1);
    w.channel.poll(t + 500000);
    CHECK(w.sent.size() == size_t(CommandChannel::kMaxAttempts));  // backing off
    w.channel.poll(t + CommandChannel::kBackoffUs);
    CHECK(w.sent.size() == size_t(CommandChannel::kMaxAttempts) + 1);
    CHECK(parse_cmd(w.sent.back())->sequence != parse_cmd(w.sent.front())->sequence);  // new transaction
}

TEST_CASE("keepalive repeats after each reply, and duplicates are re-ACKed")
{
    Wire w;
    int replies = 0;
    w.channel.add(CmdQuery::UvcUac, Bytes{}, 0, 1000000, [&](const Bytes&) { ++replies; });
    w.channel.poll(0);
    const uint16_t seq = w.last().sequence;
    w.channel.on_packet(build_cmd(CmdType::Reply, CmdQuery::UvcUac, seq), 5000);
    w.channel.on_packet(build_cmd(CmdType::Reply, CmdQuery::UvcUac, seq), 6000);  // duplicate
    CHECK(replies == 1);
    CHECK(w.sent.size() == 3);  // query + 2 reply-ACKs
    w.channel.poll(500000);
    CHECK(w.sent.size() == 3);
    w.channel.poll(1005000);
    CHECK(w.sent.size() == 4);
    CHECK(w.last().type == CmdType::Query);
    CHECK(w.last().sequence != seq);
}

TEST_CASE("queries from the pad get an ACK and an empty reply")
{
    Wire w;
    w.channel.on_packet(build_cmd(CmdType::Query, CmdQuery::Time, 77), 0);
    REQUIRE(w.sent.size() == 2);
    CHECK(parse_cmd(w.sent[0])->type == CmdType::QueryAck);
    CHECK(parse_cmd(w.sent[1])->type == CmdType::Reply);
    CHECK(parse_cmd(w.sent[1])->sequence == 77);
}

TEST_CASE("restart re-runs finished transactions with a new sequence")
{
    Wire w;
    int replies = 0;
    w.channel.add(CmdQuery::Generic, Bytes{}, 0, 0, [&](const Bytes&) { ++replies; });
    w.channel.poll(0);
    const uint16_t first = w.last().sequence;
    w.channel.on_packet(build_cmd(CmdType::Reply, CmdQuery::Generic, first), 10);
    w.channel.poll(5000000);
    CHECK(w.sent.size() == 2);  // done, nothing more
    w.channel.restart(6000000);
    w.channel.poll(6000000);
    CHECK(w.sent.size() == 3);
    CHECK(w.last().type == CmdType::Query);
    CHECK(w.last().sequence != first);
}
