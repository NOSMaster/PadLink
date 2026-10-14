// SPDX-License-Identifier: GPL-2.0-or-later
#include "video/h264.h"

#include <format>

namespace padlink {

namespace {

// Reads bits MSB-first, plus the Exp-Golomb codes H.264 headers use.
class BitReader {
public:
    explicit BitReader(ByteView data) : m_data(data) {}
    bool ok() const { return !m_overrun; }
    size_t position() const { return m_pos; }
    size_t bits_left() const { return m_data.size() * 8 - std::min(m_pos, m_data.size() * 8); }

    uint32_t u(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            if (m_pos >= m_data.size() * 8) {
                m_overrun = true;
                return 0;
            }
            v = v << 1 | ((m_data[m_pos >> 3] >> (7 - (m_pos & 7))) & 1);
            ++m_pos;
        }
        return v;
    }
    uint32_t ue()  // unsigned Exp-Golomb: N zeros, a 1, then N more bits
    {
        int zeros = 0;
        while (u(1) == 0 && ok())
            if (++zeros > 31) {
                m_overrun = true;
                return 0;
            }
        return (1u << zeros) - 1 + u(zeros);
    }
    int32_t se()
    {
        const uint32_t k = ue();
        return (k & 1) ? int32_t((k + 1) / 2) : -int32_t(k / 2);
    }

private:
    ByteView m_data;
    size_t m_pos = 0;
    bool m_overrun = false;
};

// True if RBSP data remains before the trailing stop bit.
bool more_rbsp_data(const BitReader& r, ByteView data)
{
    if (r.bits_left() == 0)
        return false;
    // Find the last 1 bit (the rbsp_stop_one_bit).
    size_t last = data.size() * 8;
    while (last > 0) {
        --last;
        if ((data[last >> 3] >> (7 - (last & 7))) & 1)
            break;
    }
    return r.position() < last;
}

} // namespace

uint32_t gamepad_slice_header(bool idr, uint8_t frame_num)
{
    return idr ? 0x25b804ffu : (0x21e003ffu | uint32_t(frame_num) << 13);
}

Bytes nal_to_rbsp(ByteView nal)
{
    Bytes out;
    out.reserve(nal.size());
    int zeros = 0;
    for (uint8_t b : nal) {
        if (zeros >= 2 && b == 0x03) {
            zeros = 0;
            continue;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}

Bytes rbsp_to_nal(ByteView rbsp)
{
    Bytes out;
    out.reserve(rbsp.size() + rbsp.size() / 64);
    int zeros = 0;
    for (uint8_t b : rbsp) {
        if (zeros >= 2 && b <= 0x03) {
            out.push_back(0x03);
            zeros = 0;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}

std::optional<SpsInfo> parse_sps(ByteView rbsp)
{
    BitReader r(rbsp);
    if ((r.u(8) & 0x1f) != 7)
        return std::nullopt;
    const uint32_t profile = r.u(8);
    r.u(16);  // constraint flags + level
    r.ue();   // sps id
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128) {
        const uint32_t chroma = r.ue();
        if (chroma == 3)
            r.u(1);
        r.ue();
        r.ue();
        r.u(1);
        if (r.u(1))  // scaling matrices: not supported (the GamePad uses none)
            return std::nullopt;
    }
    SpsInfo s;
    s.log2_max_frame_num = int(r.ue()) + 4;
    s.poc_type = int(r.ue());
    if (s.poc_type == 0) {
        s.log2_max_poc_lsb = int(r.ue()) + 4;
    } else if (s.poc_type == 1) {
        s.delta_pic_order_always_zero = r.u(1);
        r.se();
        r.se();
        const uint32_t n = r.ue();
        for (uint32_t i = 0; i < n && r.ok(); ++i)
            r.se();
    }
    r.ue();   // max_num_ref_frames
    r.u(1);   // gaps allowed
    s.width_mbs = int(r.ue()) + 1;
    s.height_mbs = int(r.ue()) + 1;
    s.frame_mbs_only = r.u(1);
    if (!r.ok())
        return std::nullopt;
    return s;
}

std::optional<PpsInfo> parse_pps(ByteView rbsp)
{
    BitReader r(rbsp);
    if ((r.u(8) & 0x1f) != 8)
        return std::nullopt;
    PpsInfo p;
    r.ue();
    r.ue();
    p.cabac = r.u(1);
    p.bottom_field_pic_order = r.u(1);
    if (r.ue() != 0)  // slice groups: not supported
        return std::nullopt;
    p.num_ref_l0_default = int(r.ue()) + 1;
    r.ue();
    p.weighted_pred = r.u(1);
    p.weighted_bipred = int(r.u(2));
    p.pic_init_qp = 26 + r.se();
    r.se();
    p.chroma_qp_offset = r.se();
    p.deblocking_control = r.u(1);
    p.constrained_intra = r.u(1);
    p.redundant_pic_cnt = r.u(1);
    if (more_rbsp_data(r, rbsp)) {
        p.transform_8x8 = r.u(1);
        // (scaling matrices / second chroma offset follow; the checks below
        // reject anything with transform_8x8 anyway)
    }
    if (!r.ok())
        return std::nullopt;
    return p;
}

std::optional<SliceHeaderInfo> parse_slice_header(ByteView rbsp, const SpsInfo& sps, const PpsInfo& pps,
                                                  std::string& error)
{
    BitReader r(rbsp);
    SliceHeaderInfo h;
    const uint32_t nal = r.u(8);
    h.nal_ref_idc = int(nal >> 5) & 3;
    h.nal_type = int(nal & 0x1f);
    if (h.nal_type != 1 && h.nal_type != 5) {
        error = std::format("not a slice NAL (type {})", h.nal_type);
        return std::nullopt;
    }
    const bool idr = h.nal_type == 5;
    h.first_mb = int(r.ue());
    h.slice_type = int(r.ue() % 5);  // 0 = P, 2 = I
    r.ue();                          // pps id
    h.frame_num = int(r.u(sps.log2_max_frame_num));
    if (!sps.frame_mbs_only) {
        error = "interlaced streams are not supported";
        return std::nullopt;
    }
    if (idr)
        h.idr_pic_id = int(r.ue());
    if (sps.poc_type == 0) {
        r.u(sps.log2_max_poc_lsb);
        if (pps.bottom_field_pic_order)
            r.se();
    } else if (sps.poc_type == 1 && !sps.delta_pic_order_always_zero) {
        r.se();
        if (pps.bottom_field_pic_order)
            r.se();
    }
    if (pps.redundant_pic_cnt)
        r.ue();
    if (h.slice_type == 0) {  // P
        h.num_ref_active = pps.num_ref_l0_default;
        if (r.u(1))
            h.num_ref_active = int(r.ue()) + 1;
        if (r.u(1)) {
            error = "reference list modification is not supported";
            return std::nullopt;
        }
    } else if (h.slice_type != 2) {
        error = std::format("slice type {} is not supported (only I and P)", h.slice_type);
        return std::nullopt;
    }
    if (pps.weighted_pred && h.slice_type == 0) {
        error = "weighted prediction is not supported";
        return std::nullopt;
    }
    if (h.nal_ref_idc != 0) {
        if (idr) {
            r.u(2);  // no_output_of_prior_pics, long_term_reference
        } else if (r.u(1)) {
            error = "adaptive reference picture marking is not supported";
            return std::nullopt;
        }
    }
    if (pps.cabac && h.slice_type != 2)
        h.cabac_init_idc = int(r.ue());
    h.qp_delta = r.se();
    if (pps.deblocking_control) {
        h.disable_deblocking = int(r.ue());
        if (h.disable_deblocking != 1) {
            h.alpha_offset = r.se() * 2;
            h.beta_offset = r.se() * 2;
        }
    }
    // CABAC slice data starts at the next byte boundary (padding bits are 1).
    while (r.position() % 8 != 0)
        if (r.u(1) != 1) {
            error = "bad cabac_alignment_one_bit";
            return std::nullopt;
        }
    if (!r.ok()) {
        error = "slice header is truncated";
        return std::nullopt;
    }
    h.data_offset = r.position() / 8;
    return h;
}

std::string check_gamepad_parameters(const SpsInfo& s, const PpsInfo& p)
{
    if (s.width_mbs != 54 || s.height_mbs != 30)
        return std::format("frame is {}x{} macroblocks, GamePad needs 54x30 (864x480)", s.width_mbs, s.height_mbs);
    if (!s.frame_mbs_only)
        return "interlaced";
    if (!p.cabac)
        return "CAVLC entropy coding (GamePad needs CABAC)";
    if (p.transform_8x8)
        return "8x8 transform enabled (GamePad has none)";
    if (!p.constrained_intra)
        return "constrained intra prediction is off (GamePad has it on)";
    if (p.chroma_qp_offset != 0)
        return std::format("chroma QP offset {} (GamePad: 0)", p.chroma_qp_offset);
    if (p.weighted_pred || p.weighted_bipred)
        return "weighted prediction";
    return {};
}

std::string check_gamepad_slice(const SliceHeaderInfo& h, const PpsInfo& p)
{
    if (h.first_mb != 0)
        return "more than one slice per frame";
    if (p.pic_init_qp + h.qp_delta != kGamepadQp)
        return std::format("slice QP {} (GamePad decodes with {})", p.pic_init_qp + h.qp_delta, kGamepadQp);
    if (h.nal_ref_idc == 0)
        return "non-reference frame";
    if (h.slice_type == 0 && h.num_ref_active != 1)
        return std::format("{} active references (GamePad: 1)", h.num_ref_active);
    if (h.cabac_init_idc != 0)
        return std::format("cabac_init_idc {} (GamePad: 0)", h.cabac_init_idc);
    if (h.disable_deblocking != 0 || h.alpha_offset != 0 || h.beta_offset != 0)
        return "non-default deblocking";
    return {};
}

Bytes reconstruct_annexb(ByteView slice_data, bool idr, uint8_t frame_num, bool with_parameter_sets)
{
    static constexpr uint8_t start[] = {0, 0, 0, 1};
    Bytes out;
    if (with_parameter_sets) {
        put_bytes(out, start);
        put_bytes(out, kGamepadSps);
        put_bytes(out, start);
        put_bytes(out, kGamepadPps);
    }
    put_bytes(out, start);
    Bytes rbsp;
    put_be32(rbsp, gamepad_slice_header(idr, frame_num));
    put_bytes(rbsp, slice_data);
    put_bytes(out, rbsp_to_nal(rbsp));
    return out;
}

} // namespace padlink
