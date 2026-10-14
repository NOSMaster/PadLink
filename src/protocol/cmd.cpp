// SPDX-License-Identifier: GPL-2.0-or-later
#include "protocol/cmd.h"

namespace padlink {

namespace {

void put_le16(Bytes& out, uint16_t v)
{
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
}

void write_le16(Bytes& b, size_t at, uint16_t v)
{
    b[at] = uint8_t(v);
    b[at + 1] = uint8_t(v >> 8);
}

} // namespace

Bytes build_cmd(CmdType type, CmdQuery query, uint16_t sequence, ByteView payload)
{
    Bytes out;
    put_le16(out, uint16_t(type));
    put_le16(out, uint16_t(query));
    put_le16(out, uint16_t(payload.size()));
    put_le16(out, sequence);
    put_bytes(out, payload);
    return out;
}

std::optional<CmdPacket> parse_cmd(ByteView p)
{
    if (p.size() < kCmdHeaderSize)
        return std::nullopt;
    const uint16_t type = read_le16(&p[0]);
    const uint16_t query = read_le16(&p[2]);
    const uint16_t size = read_le16(&p[4]);
    if (type > 3 || query > 2 || p.size() != kCmdHeaderSize + size)
        return std::nullopt;
    return CmdPacket{CmdType(type), CmdQuery(query), read_le16(&p[6]),
                     Bytes(p.begin() + kCmdHeaderSize, p.end())};
}

Bytes build_generic_query(uint8_t service, uint8_t method, ByteView args)
{
    // magic, version, 12-bit transaction id + 12-bit fragment id (0x000/0x800
    // as sent by a real console), flags 0x40 = query, service, method,
    // BE error code, BE argument length.
    Bytes out{0x7e, 0x01, 0x00, 0x08, 0x00, 0x40, service, method, 0x00, 0x00};
    put_be16(out, uint16_t(args.size()));
    put_bytes(out, args);
    return out;
}

std::optional<GenericReply> parse_generic_reply(ByteView b)
{
    if (b.size() < kGenericHeaderSize || b[0] != 0x7e)
        return std::nullopt;
    const uint16_t size = read_be16(&b[10]);
    if (b.size() != kGenericHeaderSize + size)
        return std::nullopt;
    return GenericReply{b[6], b[7], read_be16(&b[8]), Bytes(b.begin() + kGenericHeaderSize, b.end())};
}

Bytes build_uvc_uac_query()
{
    Bytes q(48, 0);
    write_le16(q, 12, 16000);  // microphone sample rate
    return q;
}

void sync_uvc_uac_query(Bytes& q, ByteView reply)
{
    if (q.size() < 48 || reply.size() < 16)
        return;
    q[0] = 1;                                // changes 0 -> 1 after the first exchange
    q[4] = reply[12] != 0;                   // mic enable
    write_le16(q, 6, read_le16(&reply[0]));  // mic volume
    write_le16(q, 8, read_le16(&reply[2]));  // mic jack volume
    q[16] = reply[13];
    q[17] = reply[14] != 0;
}

size_t CommandChannel::add(CmdQuery query, Bytes payload, int64_t start_us, int64_t repeat_us, ReplyHandler on_reply)
{
    Transaction t;
    t.query = query;
    t.payload = std::move(payload);
    t.on_reply = std::move(on_reply);
    t.repeat_us = repeat_us;
    t.next_due = start_us;
    m_tx.push_back(std::move(t));
    return m_tx.size() - 1;
}

void CommandChannel::send_attempt(Transaction& t, int64_t now)
{
    if (t.stage == Stage::Idle) {
        t.sequence = m_next_sequence++;
        t.attempts = 0;
        t.stage = Stage::AwaitingAck;
    } else {
        ++m_stats.retries;
    }
    ++t.attempts;
    ++m_stats.sent;
    t.next_due = now + kRetryUs;
    m_send(build_cmd(CmdType::Query, t.query, t.sequence, t.payload));
}

void CommandChannel::restart(int64_t now)
{
    for (auto& t : m_tx) {
        t.stage = Stage::Idle;
        t.attempts = 0;
        t.next_due = now;
    }
}

void CommandChannel::poll(int64_t now)
{
    for (auto& t : m_tx) {
        if (t.stage == Stage::Done || now < t.next_due)
            continue;
        switch (t.stage) {
        case Stage::Idle:  // time for a new query (first or keepalive)
            send_attempt(t, now);
            break;
        case Stage::AwaitingAck:  // no ACK yet: resend, or give up for a while
            if (t.attempts < kMaxAttempts) {
                send_attempt(t, now);
            } else {
                ++m_stats.timeouts;
                t.stage = Stage::Idle;
                t.next_due = now + kBackoffUs;
            }
            break;
        case Stage::AwaitingReply:  // ACKed but the reply never came: start over
            ++m_stats.timeouts;
            t.stage = Stage::Idle;
            send_attempt(t, now);
            break;
        case Stage::Done:
            break;
        }
    }
}

void CommandChannel::on_packet(ByteView packet, int64_t now)
{
    const auto p = parse_cmd(packet);
    if (!p)
        return;
    switch (p->type) {
    case CmdType::QueryAck:
        for (auto& t : m_tx)
            if (t.stage == Stage::AwaitingAck && t.sequence == p->sequence && t.query == p->query) {
                t.stage = Stage::AwaitingReply;
                t.next_due = now + kReplyTimeoutUs;
            }
        break;
    case CmdType::Reply:
        // Always acknowledge, even duplicates, or the pad keeps resending.
        m_send(build_cmd(CmdType::ReplyAck, p->query, p->sequence));
        for (auto& t : m_tx) {
            if ((t.stage == Stage::AwaitingAck || t.stage == Stage::AwaitingReply) &&
                t.sequence == p->sequence && t.query == p->query) {
                ++m_stats.replies;
                t.stage = t.repeat_us > 0 ? Stage::Idle : Stage::Done;
                t.next_due = now + t.repeat_us;
                if (t.on_reply)
                    t.on_reply(p->payload);
            }
        }
        break;
    case CmdType::Query:
        // The pad asking us something: acknowledge it and reply empty.
        m_send(build_cmd(CmdType::QueryAck, p->query, p->sequence));
        m_send(build_cmd(CmdType::Reply, p->query, p->sequence));
        break;
    case CmdType::ReplyAck:
        break;
    }
}

} // namespace padlink
