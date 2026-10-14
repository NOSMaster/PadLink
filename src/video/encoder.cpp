// SPDX-License-Identifier: GPL-2.0-or-later
#include "video/encoder.h"

#include "video/h264.h"

#include <algorithm>
#include <cstdint>
#include <format>

extern "C" {
#include <x264.h>
}

namespace padlink {

struct Encoder::Impl {
    x264_t* x264 = nullptr;
    SpsInfo sps;
    PpsInfo pps;
    int64_t pts = 0;
    ~Impl()
    {
        if (x264)
            x264_encoder_close(x264);
    }
};

namespace {

// x264 output starts with a 3- or 4-byte start code (00 00 01 / 00 00 00 01).
ByteView strip_start_code(const x264_nal_t& nal)
{
    ByteView v(nal.p_payload, size_t(nal.i_payload));
    while (!v.empty() && v[0] == 0)
        v = v.subspan(1);
    if (!v.empty() && v[0] == 1)
        v = v.subspan(1);
    return v;
}

} // namespace

std::array<Bytes, kVideoChunks> split_into_chunks(ByteView data, ChunkSplit mode,
                                                  const std::array<size_t, kVideoChunks - 1>& ends)
{
    // Every chunk needs at least one byte. Zero bytes after the end of the
    // slice are ignored by H.264 decoders (trailing_zero_8bits).
    Bytes padded(data.begin(), data.end());
    if (padded.size() < kVideoChunks)
        padded.resize(kVideoChunks, 0);
    const size_t n = padded.size();

    std::array<size_t, kVideoChunks + 1> cut{};
    for (size_t i = 1; i < kVideoChunks; ++i) {
        switch (mode) {
        case ChunkSplit::Rows: cut[i] = ends[i - 1] + kChunkSlackBytes; break;
        case ChunkSplit::Late: cut[i] = n - (kVideoChunks - i); break;
        case ChunkSplit::Even: cut[i] = n * i / kVideoChunks; break;
        }
        // Keep every chunk at least one byte long and inside the data.
        cut[i] = std::clamp(cut[i], cut[i - 1] + 1, n - (kVideoChunks - i));
    }
    cut[kVideoChunks] = n;

    std::array<Bytes, kVideoChunks> chunks;
    for (size_t i = 0; i < kVideoChunks; ++i)
        chunks[i].assign(padded.begin() + ptrdiff_t(cut[i]), padded.begin() + ptrdiff_t(cut[i + 1]));
    return chunks;
}

std::unique_ptr<Encoder> Encoder::create(std::string& error, VideoMode mode)
{
    x264_param_t p;
    if (x264_param_default_preset(&p, "superfast", "zerolatency") < 0) {
        error = "x264: preset not available";
        return nullptr;
    }
    p.i_threads = 1;            // one thread: no frame delay, one slice
    p.b_sliced_threads = 0;
    p.i_width = kVideoWidth;
    p.i_height = kVideoHeight;
    p.i_csp = X264_CSP_I420;
    p.i_fps_num = 60000;
    p.i_fps_den = 1001;
    p.b_vfr_input = 0;
    p.i_level_idc = 32;         // GamePad SPS: level 3.2

    // We decide when IDRs happen (start, GamePad request, periodic).
    p.i_keyint_max = X264_KEYINT_MAX_INFINITE;
    p.i_scenecut_threshold = 0;
    p.b_intra_refresh = 0;

    // Everything below must match the GamePad's fixed SPS/PPS/slice header.
    p.i_bframe = 0;
    p.i_frame_reference = 1;
    p.b_cabac = 1;
    p.i_cabac_init_idc = 0;
    p.b_constrained_intra = 1;
    p.b_deblocking_filter = 1;
    p.i_deblocking_filter_alphac0 = 0;
    p.i_deblocking_filter_beta = 0;
    p.analyse.b_transform_8x8 = 0;
    p.analyse.i_weighted_pred = X264_WEIGHTP_NONE;
    p.analyse.b_psy = 0;            // psy tuning would change the chroma QP offset
    p.analyse.i_chroma_qp_offset = 0;
    p.analyse.inter &= ~X264_ANALYSE_PSUB16x16;  // no 8x8 sub-partitions (as libdrc)
    p.rc.i_rc_method = X264_RC_CQP; // fixed QP 32: the GamePad's slice QP
    p.rc.i_qp_constant = kGamepadQp;
    p.rc.f_ip_factor = 1.0f;        // otherwise I-frames get a lower QP
    p.rc.f_pb_factor = 1.0f;
    p.rc.i_aq_mode = X264_AQ_NONE;
    p.rc.b_mb_tree = 0;
    p.rc.i_lookahead = 0;
    p.i_sync_lookahead = 0;
    if (mode == VideoMode::Fullpel) {
        p.analyse.i_subpel_refine = 0;  // integer motion vectors only
        p.analyse.i_me_method = X264_ME_DIA;
        p.analyse.i_me_range = 4;
    }
    p.i_slice_count = 1;            // one slice per frame
    p.b_padlink_row_offsets = 1;    // our x264 patch: report where each MB row ends
    p.i_slice_max_size = 0;
    p.i_slice_max_mbs = 0;
    p.b_repeat_headers = 0;
    p.b_annexb = 1;
    p.b_aud = 0;
    p.i_log_level = X264_LOG_WARNING;
    if (x264_param_apply_profile(&p, "high") < 0) {
        error = "x264: cannot apply High profile";
        return nullptr;
    }

    std::unique_ptr<Encoder> enc(new Encoder());
    enc->m_impl = std::make_unique<Impl>();
    enc->m_impl->x264 = x264_encoder_open(&p);
    if (!enc->m_impl->x264) {
        error = "x264_encoder_open failed";
        return nullptr;
    }

    // Check that x264's parameter sets decode exactly like the GamePad's.
    x264_nal_t* nals = nullptr;
    int count = 0;
    if (x264_encoder_headers(enc->m_impl->x264, &nals, &count) < 0) {
        error = "x264_encoder_headers failed";
        return nullptr;
    }
    std::optional<SpsInfo> sps;
    std::optional<PpsInfo> pps;
    for (int i = 0; i < count; ++i) {
        const Bytes rbsp = nal_to_rbsp(strip_start_code(nals[i]));
        if (nals[i].i_type == NAL_SPS)
            sps = parse_sps(rbsp);
        else if (nals[i].i_type == NAL_PPS)
            pps = parse_pps(rbsp);
    }
    if (!sps || !pps) {
        error = "cannot parse x264's SPS/PPS";
        return nullptr;
    }
    if (const std::string why = check_gamepad_parameters(*sps, *pps); !why.empty()) {
        error = "x264 configuration does not match the GamePad: " + why;
        return nullptr;
    }
    enc->m_impl->sps = *sps;
    enc->m_impl->pps = *pps;
    return enc;
}

Encoder::~Encoder() = default;

std::optional<EncodedFrame> Encoder::encode(const uint8_t* i420, bool force_idr, std::string& error)
{
    x264_picture_t in, out;
    x264_picture_init(&in);
    in.img.i_csp = X264_CSP_I420;
    in.img.i_plane = 3;
    // x264 wants non-const pointers but does not modify the input.
    auto* y = const_cast<uint8_t*>(i420);
    in.img.plane[0] = y;
    in.img.plane[1] = y + kVideoWidth * kVideoHeight;
    in.img.plane[2] = in.img.plane[1] + kVideoWidth * kVideoHeight / 4;
    in.img.i_stride[0] = kVideoWidth;
    in.img.i_stride[1] = in.img.i_stride[2] = kVideoWidth / 2;
    in.i_type = force_idr ? X264_TYPE_IDR : X264_TYPE_P;
    in.i_pts = m_impl->pts++;
    std::array<int, X264_PADLINK_MAX_ROWS> row_end{};
    row_end.fill(-1);
    in.opaque = row_end.data();

    x264_nal_t* nals = nullptr;
    int count = 0;
    if (x264_encoder_encode(m_impl->x264, &nals, &count, &in, &out) < 0) {
        error = "x264_encoder_encode failed";
        return std::nullopt;
    }
    for (int i = 0; i < count; ++i) {
        if (nals[i].i_type != NAL_SLICE && nals[i].i_type != NAL_SLICE_IDR)
            continue;  // SEI etc.
        const Bytes rbsp = nal_to_rbsp(strip_start_code(nals[i]));
        const auto header = parse_slice_header(rbsp, m_impl->sps, m_impl->pps, error);
        if (!header)
            return std::nullopt;
        if (const std::string why = check_gamepad_slice(*header, m_impl->pps); !why.empty()) {
            error = "x264 slice does not match the GamePad: " + why;
            return std::nullopt;
        }
        EncodedFrame f;
        f.idr = header->nal_type == 5;
        // Rows 5, 11, 17, 23 end chunks 0..3 (6 macroblock rows per chunk).
        std::array<size_t, kVideoChunks - 1> ends{};
        for (size_t k = 0; k < ends.size(); ++k) {
            const int end = row_end[6 * k + 5];
            if (m_split == ChunkSplit::Rows && end < 0) {
                error = "x264 did not report macroblock row offsets (unpatched x264?)";
                return std::nullopt;
            }
            ends[k] = size_t(std::max(end, 0));
        }
        f.chunks = split_into_chunks(ByteView(rbsp).subspan(header->data_offset), m_split, ends);
        return f;
    }
    error = "x264 produced no slice (frame delayed?)";
    return std::nullopt;
}

} // namespace padlink
