// SPDX-License-Identifier: GPL-2.0-or-later
// CMD: a small request/reply protocol on UDP 50023 <-> 50123 (PROTOCOL.md §6).
#pragma once

#include "protocol/bytes.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace padlink {

enum class CmdType : uint16_t { Query = 0, QueryAck = 1, Reply = 2, ReplyAck = 3 };
enum class CmdQuery : uint16_t { Generic = 0, UvcUac = 1, Time = 2 };

struct CmdPacket {
    CmdType type{};
    CmdQuery query{};
    uint16_t sequence = 0;
    Bytes payload;
};

inline constexpr size_t kCmdHeaderSize = 8;
inline constexpr size_t kGenericHeaderSize = 12;

Bytes build_cmd(CmdType type, CmdQuery query, uint16_t sequence, ByteView payload = {});
std::optional<CmdPacket> parse_cmd(ByteView packet);

// Generic command body: 12-byte header (magic 0x7E ...) + arguments.
Bytes build_generic_query(uint8_t service, uint8_t method, ByteView args = {});

struct GenericReply {
    uint8_t service = 0, method = 0;
    uint16_t error = 0;
    Bytes data;
};
std::optional<GenericReply> parse_generic_reply(ByteView body);

// UVC/UAC (camera/microphone settings) query, used as a keepalive. After the
// first reply the console echoes some reply fields back [observed].
Bytes build_uvc_uac_query();
void sync_uvc_uac_query(Bytes& query, ByteView reply);

// Known commands.
inline constexpr uint8_t kServicePeripheral = 5;
inline constexpr uint8_t kMethodReadUicConfig = 0x06;   // reply: 4 + 768 bytes
inline constexpr uint8_t kMethodSetBrightness = 0x14;  // arg: 1 byte, 1..5

// Drives CMD transactions: sends queries, retries until acknowledged, ACKs
// every reply and repeats keepalive queries. Time is passed in (µs), so the
// logic can be tested without sockets or sleeping.
class CommandChannel {
public:
    using Sender = std::function<void(const Bytes&)>;
    using ReplyHandler = std::function<void(const Bytes& reply_payload)>;

    struct Stats {
        uint64_t sent = 0, retries = 0, replies = 0, timeouts = 0;
    };

    explicit CommandChannel(Sender sender) : m_send(std::move(sender)) {}

    // Adds a transaction. repeat_us > 0 makes it repeat that long after each
    // reply (keepalive). Returns an id for update_payload().
    size_t add(CmdQuery query, Bytes payload, int64_t start_us, int64_t repeat_us, ReplyHandler on_reply);
    void update_payload(size_t id, Bytes payload) { m_tx[id].payload = std::move(payload); }

    void on_packet(ByteView packet, int64_t now_us);
    // Starts every transaction over (e.g. after the GamePad reconnects).
    void restart(int64_t now_us);
    void poll(int64_t now_us);
    const Stats& stats() const { return m_stats; }

    static constexpr int64_t kRetryUs = 100000;  // 100 ms between attempts
    static constexpr int kMaxAttempts = 10;
    static constexpr int64_t kBackoffUs = 1000000;  // after giving up
    static constexpr int64_t kReplyTimeoutUs = 1000000;  // ACKed, waiting for the reply

private:
    enum class Stage { Idle, AwaitingAck, AwaitingReply, Done };
    struct Transaction {
        CmdQuery query{};
        Bytes payload;
        ReplyHandler on_reply;
        int64_t repeat_us = 0;
        Stage stage = Stage::Idle;
        uint16_t sequence = 0;
        int attempts = 0;
        int64_t next_due = 0;
    };
    void send_attempt(Transaction& t, int64_t now_us);

    Sender m_send;
    std::vector<Transaction> m_tx;
    uint16_t m_next_sequence = 1;
    Stats m_stats;
};

} // namespace padlink
