// SPDX-License-Identifier: GPL-2.0-or-later
// H.264 encoder for the GamePad, built on x264 (GPL-2.0-or-later).
#pragma once

#include "protocol/bytes.h"
#include "protocol/vstrm.h"

#include <array>
#include <memory>
#include <optional>
#include <string>

namespace padlink {

inline constexpr int kVideoWidth = 864;   // coded width; the GamePad shows 854
inline constexpr int kVideoHeight = 480;
inline constexpr size_t kI420FrameSize = size_t(kVideoWidth) * kVideoHeight * 3 / 2;

struct EncodedFrame {
    bool idr = false;
    std::array<Bytes, kVideoChunks> chunks;  // slice data, split into 5 parts
    size_t size() const
    {
        size_t n = 0;
        for (const auto& c : chunks)
            n += c.size();
        return n;
    }
};

// How the single slice is cut into the five chunks.
//  Rows: at the byte where each group of 6 macroblock rows ends (reported by
//        our patched x264), plus 2 bytes of slack, like a real console.
//  Late: everything in chunk 0 (experiment: fails for big frames on hardware)
//  Even: five equal parts (experiment)
enum class ChunkSplit { Rows, Late, Even };
inline constexpr size_t kChunkSlackBytes = 2;

// Diagnostic encoder modes, to find which H.264 feature the GamePad decodes
// differently from x264 (PROTOCOL.md §4.5).
enum class VideoMode {
    Normal,   // x264 "superfast" motion search (sub-pixel motion vectors)
    Fullpel,  // whole-pixel motion vectors only
};

class Encoder {
public:
    // Returns nullptr and sets `error` if x264 cannot be configured to match
    // the GamePad's decoder.
    static std::unique_ptr<Encoder> create(std::string& error, VideoMode mode = VideoMode::Normal);
    ~Encoder();

    // `i420`: 864x480 planar Y, then U, then V (kI420FrameSize bytes).
    std::optional<EncodedFrame> encode(const uint8_t* i420, bool force_idr, std::string& error);

    void set_chunk_split(ChunkSplit s) { m_split = s; }

private:
    Encoder() = default;
    struct Impl;  // hides x264 types from everything that includes this header
    std::unique_ptr<Impl> m_impl;
    ChunkSplit m_split = ChunkSplit::Rows;
};

// Pure helper (unit tested). `chunk_ends`: for ChunkSplit::Rows, the byte
// offset where macroblock rows 5, 11, 17 and 23 end in `slice_data`.
std::array<Bytes, kVideoChunks> split_into_chunks(ByteView slice_data, ChunkSplit mode,
                                                  const std::array<size_t, kVideoChunks - 1>& chunk_ends = {});

} // namespace padlink
