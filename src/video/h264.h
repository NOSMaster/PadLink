// SPDX-License-Identifier: GPL-2.0-or-later
// H.264 helpers for the GamePad's fixed decoder configuration (PROTOCOL.md §4.1).
//
// The GamePad never receives SPS, PPS or slice headers: it assumes its own.
// So we encode normally, check that the encoder's headers mean the same as the
// GamePad's, and transmit only the slice *data* that follows the header.
#pragma once

#include "protocol/bytes.h"

#include <array>
#include <optional>
#include <string>

namespace padlink {

// The GamePad's built-in parameter sets (as documented by libdrc): High
// profile, 864x480 cropped to 854x480, CABAC, POC type 2, 8-bit frame_num,
// 1 reference, QP 32, constrained intra prediction, no 8x8 transform.
inline constexpr std::array<uint8_t, 11> kGamepadSps{0x67, 0x64, 0x00, 0x20, 0xac, 0x2b, 0x40, 0x6c, 0x1e, 0xf3, 0x68};
inline constexpr std::array<uint8_t, 5> kGamepadPps{0x68, 0xee, 0x06, 0x0c, 0xe8};
inline constexpr int kGamepadQp = 32;

// The slice header the GamePad assumes, as one 32-bit word (NAL header
// included, CABAC alignment bits at the end).
uint32_t gamepad_slice_header(bool idr, uint8_t frame_num);

// NAL payload <-> RBSP: removes / inserts emulation-prevention 0x03 bytes.
Bytes nal_to_rbsp(ByteView nal);
Bytes rbsp_to_nal(ByteView rbsp);

struct SpsInfo {
    int log2_max_frame_num = 0, poc_type = 0, log2_max_poc_lsb = 0;
    bool delta_pic_order_always_zero = false, frame_mbs_only = true;
    int width_mbs = 0, height_mbs = 0;
};
struct PpsInfo {
    bool cabac = false, bottom_field_pic_order = false, weighted_pred = false;
    bool deblocking_control = false, constrained_intra = false, redundant_pic_cnt = false;
    bool transform_8x8 = false;
    int num_ref_l0_default = 1, weighted_bipred = 0, pic_init_qp = 26, chroma_qp_offset = 0;
};
struct SliceHeaderInfo {
    int nal_ref_idc = 0, nal_type = 0, first_mb = 0, slice_type = 0, frame_num = 0, idr_pic_id = 0;
    int num_ref_active = 1, qp_delta = 0, cabac_init_idc = 0;
    int disable_deblocking = 0, alpha_offset = 0, beta_offset = 0;
    size_t data_offset = 0;  // byte offset of slice_data() in the RBSP
};

// All three take an RBSP *including* the 1-byte NAL header.
std::optional<SpsInfo> parse_sps(ByteView rbsp);
std::optional<PpsInfo> parse_pps(ByteView rbsp);
std::optional<SliceHeaderInfo> parse_slice_header(ByteView rbsp, const SpsInfo& sps, const PpsInfo& pps, std::string& error);

// Empty string if the encoder's parameter sets decode identically to the
// GamePad's; otherwise a description of the first difference.
std::string check_gamepad_parameters(const SpsInfo& sps, const PpsInfo& pps);
std::string check_gamepad_slice(const SliceHeaderInfo& slice, const PpsInfo& pps);

// Rebuilds a normal Annex-B H.264 frame from what the GamePad receives, the
// same way a GamePad-side decoder (vanilla) does. For tests and debugging.
Bytes reconstruct_annexb(ByteView slice_data, bool idr, uint8_t frame_num, bool with_parameter_sets);

} // namespace padlink
