// SPDX-License-Identifier: GPL-2.0-or-later
#include "doctest.h"
#include "protocol/vstrm.h"

using namespace padlink;

TEST_CASE("VSTRM header matches the documented real-console example")
{
    // seq 0x321, ts 0x12345678, first packet of an IDR, 1694-byte payload,
    // chunk ends in this packet. Expected: f3 21 ee 9e 12 34 56 78 80 82 00 83 85 06 00 00
    VstrmHeader h;
    h.sequence = 0x321;
    h.init = h.frame_begin = h.chunk_end = h.idr = true;
    h.timestamp = 0x12345678;
    const Bytes payload(1694, 0xab);
    const Bytes p = build_vstrm_packet(h, payload);
    REQUIRE(p.size() == 16 + 1694);
    CHECK(Bytes(p.begin(), p.begin() + 16) ==
          Bytes{0xf3, 0x21, 0xee, 0x9e, 0x12, 0x34, 0x56, 0x78, 0x80, 0x82, 0x00, 0x83, 0x85, 0x06, 0x00, 0x00});

    const auto back = parse_vstrm_packet(p);
    REQUIRE(back);
    CHECK(back->header.sequence == 0x321);
    CHECK(back->header.init);
    CHECK(back->header.frame_begin);
    CHECK(back->header.chunk_end);
    CHECK_FALSE(back->header.frame_end);
    CHECK(back->header.idr);
    CHECK(back->header.timestamp == 0x12345678);
    CHECK(back->payload == payload);
}

TEST_CASE("P-frame options and flags")
{
    VstrmHeader h;
    h.sequence = 5;
    h.frame_end = true;
    const Bytes p = build_vstrm_packet(h, Bytes{1, 2, 3});
    CHECK(Bytes(p.begin() + 8, p.begin() + 16) == Bytes{0x82, 0x00, 0x83, 0x85, 0x06, 0x00, 0x00, 0x00});
    CHECK(p[2] == (0x10 | 0x08));
    const auto back = parse_vstrm_packet(p);
    REQUIRE(back);
    CHECK_FALSE(back->header.idr);
    CHECK(back->header.frame_end);
}

TEST_CASE("packetizer: chunk/frame flags, 1694-byte limit, wrapping sequence")
{
    std::array<Bytes, kVideoChunks> chunks;
    chunks[0] = Bytes(1694 * 2 + 10, 1);  // 3 packets
    for (size_t i = 1; i < kVideoChunks; ++i)
        chunks[i] = Bytes(1, uint8_t(i + 1));
    uint16_t seq = 1022;
    const auto packets = packetize_frame(chunks, true, true, 777, seq);
    REQUIRE(packets.size() == 7);
    CHECK(seq == 5);  // 1022 + 7 wraps at 1024

    std::vector<ParsedVstrm> parsed;
    for (const auto& p : packets) {
        auto v = parse_vstrm_packet(p);
        REQUIRE(v);
        parsed.push_back(*v);
    }
    CHECK(parsed[0].header.sequence == 1022);
    CHECK(parsed[1].header.sequence == 1023);
    CHECK(parsed[2].header.sequence == 0);
    CHECK(parsed[0].header.frame_begin);
    CHECK_FALSE(parsed[1].header.frame_begin);
    CHECK(parsed[0].payload.size() == 1694);
    CHECK(parsed[2].payload.size() == 10);
    // chunk_end on the last packet of each chunk only; frame_end only at the very end
    const bool chunk_ends[] = {false, false, true, true, true, true, true};
    for (size_t i = 0; i < packets.size(); ++i) {
        CHECK(parsed[i].header.chunk_end == chunk_ends[i]);
        CHECK(parsed[i].header.frame_end == (i == packets.size() - 1));
        CHECK(parsed[i].header.timestamp == 777);
        CHECK(parsed[i].header.idr);
        CHECK(parsed[i].header.init);
    }
}

TEST_CASE("video-format message layout")
{
    const Bytes f = build_video_format(0x12345678);
    CHECK(f == Bytes{0x04, 0x00, 0x00, 0x00, 0x10, 0x10, 0x81, 0x08, 0x78, 0x56, 0x34, 0x12,
                     0x80, 0x3e, 0x00, 0x00, 0x80, 0x3e, 0x00, 0x00, 0x08, 0x52, 0x00, 0x00,
                     0x08, 0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
}

TEST_CASE("malformed VSTRM packets are rejected")
{
    CHECK_FALSE(parse_vstrm_packet(Bytes(10, 0)));
    Bytes p = build_vstrm_packet(VstrmHeader{}, Bytes{1, 2});
    p.push_back(0);  // length no longer matches
    CHECK_FALSE(parse_vstrm_packet(p));
    p.pop_back();
    p[0] = 0x03;     // wrong magic
    CHECK_FALSE(parse_vstrm_packet(p));
}
