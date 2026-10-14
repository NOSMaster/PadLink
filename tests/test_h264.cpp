// SPDX-License-Identifier: GPL-2.0-or-later
#include "doctest.h"
#include "video/encoder.h"
#include "video/frame_source.h"
#include "video/h264.h"

using namespace padlink;

TEST_CASE("the GamePad's own SPS/PPS parse to the documented configuration")
{
    const auto sps = parse_sps(kGamepadSps);
    const auto pps = parse_pps(kGamepadPps);
    REQUIRE(sps);
    REQUIRE(pps);
    CHECK(sps->width_mbs == 54);
    CHECK(sps->height_mbs == 30);
    CHECK(sps->log2_max_frame_num == 8);
    CHECK(sps->poc_type == 2);
    CHECK(pps->cabac);
    CHECK(pps->pic_init_qp == 32);
    CHECK(pps->constrained_intra);
    CHECK_FALSE(pps->transform_8x8);
    CHECK(pps->chroma_qp_offset == 0);
    CHECK(pps->deblocking_control);
    CHECK(check_gamepad_parameters(*sps, *pps).empty());
}

TEST_CASE("the GamePad's fixed slice headers parse with its own SPS/PPS")
{
    const auto sps = *parse_sps(kGamepadSps);
    const auto pps = *parse_pps(kGamepadPps);
    std::string error;

    Bytes idr;
    put_be32(idr, gamepad_slice_header(true, 0));
    CHECK(idr == Bytes{0x25, 0xb8, 0x04, 0xff});
    const auto i = parse_slice_header(idr, sps, pps, error);
    REQUIRE_MESSAGE(i, error);
    CHECK(i->nal_type == 5);
    CHECK(i->slice_type == 2);
    CHECK(i->qp_delta == 0);
    CHECK(i->data_offset == 4);  // slice data starts right after the 4-byte word
    CHECK(check_gamepad_slice(*i, pps).empty());

    for (int fn : {1, 37, 255}) {
        Bytes p;
        put_be32(p, gamepad_slice_header(false, uint8_t(fn)));
        const auto h = parse_slice_header(p, sps, pps, error);
        REQUIRE_MESSAGE(h, error);
        CHECK(h->nal_type == 1);
        CHECK(h->slice_type == 0);
        CHECK(h->frame_num == fn);
        CHECK(h->num_ref_active == 1);
        CHECK(h->data_offset == 4);
        CHECK(check_gamepad_slice(*h, pps).empty());
    }
}

TEST_CASE("emulation prevention round-trips")
{
    const Bytes rbsp{0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x02, 0xff, 0x00, 0x00};
    const Bytes nal = rbsp_to_nal(rbsp);
    CHECK(nal == Bytes{0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x03, 0x03, 0x00, 0x00, 0x03, 0x02, 0xff, 0x00, 0x00});
    CHECK(nal_to_rbsp(nal) == rbsp);
}

TEST_CASE("chunk split: late boundaries and every chunk non-empty")
{
    Bytes data(1000);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = uint8_t(i);
    const auto late = split_into_chunks(data, ChunkSplit::Late);
    CHECK(late[0].size() == 996);
    for (size_t i = 1; i < 5; ++i)
        CHECK(late[i].size() == 1);
    Bytes joined;
    for (const auto& c : late)
        put_bytes(joined, c);
    CHECK(joined == data);

    const auto even = split_into_chunks(data, ChunkSplit::Even);
    for (const auto& c : even)
        CHECK(c.size() == 200);

    const auto tiny = split_into_chunks(Bytes{7, 8}, ChunkSplit::Late);
    for (const auto& c : tiny)
        CHECK(c.size() == 1);
}

TEST_CASE("x264 is configured to match the GamePad and frames pass the slice checks")
{
    std::string error;
    auto enc = Encoder::create(error);
    REQUIRE_MESSAGE(enc, error);
    TestPattern pattern;
    Bytes frame(kI420FrameSize);
    for (int i = 0; i < 5; ++i) {
        pattern.latest(frame.data());
        const auto f = enc->encode(frame.data(), i == 0, error);
        REQUIRE_MESSAGE(f, error);
        CHECK(f->idr == (i == 0));
        for (const auto& c : f->chunks)
            CHECK_FALSE(c.empty());
    }
}

TEST_CASE("chunk split at reported row ends, with slack and bounds")
{
    Bytes data(1000, 0x55);
    const auto c = split_into_chunks(data, ChunkSplit::Rows, {100, 300, 600, 900});
    CHECK(c[0].size() == 102);  // 100 + 2 bytes slack
    CHECK(c[1].size() == 200);
    CHECK(c[2].size() == 300);
    CHECK(c[3].size() == 300);
    CHECK(c[4].size() == 98);
    // Ends past the data (tiny frame): every chunk still gets a byte.
    const auto t = split_into_chunks(Bytes(6, 1), ChunkSplit::Rows, {5, 5, 5, 5});
    for (const auto& ch : t)
        CHECK_FALSE(ch.empty());
}

TEST_CASE("patched x264 reports increasing row ends for a real frame")
{
    std::string error;
    auto enc = Encoder::create(error);
    REQUIRE_MESSAGE(enc, error);
    TestPattern pattern;
    Bytes frame(kI420FrameSize);
    pattern.latest(frame.data());
    const auto f = enc->encode(frame.data(), true, error);
    REQUIRE_MESSAGE(f, error);
    // The pattern's colour bars fill the top 320 rows: chunks 0..2 carry most bytes.
    CHECK(f->chunks[0].size() > 10);
    CHECK(f->chunks[1].size() > 10);
}
