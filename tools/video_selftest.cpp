// SPDX-License-Identifier: GPL-2.0-or-later
// Encodes the test pattern exactly as padlinkd would send it, then rebuilds a
// normal .h264 file the way a GamePad-side decoder does (fixed SPS/PPS/slice
// headers + the chunks we transmit). If FFmpeg decodes that file cleanly and
// it looks like the input, our slice data matches the GamePad's assumptions.
//
// usage: padlink-video-selftest <out.h264> <out-input.yuv> [frames] [rows|late|even|fullpel]

#include "video/encoder.h"
#include "video/frame_source.h"
#include "video/h264.h"

#include <cstdio>
#include <fstream>
#include <string>

using namespace padlink;

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <out.h264> <out-input.yuv> [frames] [rows|late|even|fullpel]\n", argv[0]);
        return 2;
    }
    const int frames = argc > 3 ? std::stoi(argv[3]) : 120;
    const std::string variant = argc > 4 ? argv[4] : "rows";
    const VideoMode mode = variant == "fullpel" ? VideoMode::Fullpel : VideoMode::Normal;
    std::string error;
    auto enc = Encoder::create(error, mode);
    if (!enc) {
        std::fprintf(stderr, "encoder: %s\n", error.c_str());
        return 1;
    }
    if (variant == "even")
        enc->set_chunk_split(ChunkSplit::Even);
    if (variant == "late")
        enc->set_chunk_split(ChunkSplit::Late);

    std::ofstream h264(argv[1], std::ios::binary), yuv(argv[2], std::ios::binary);
    TestPattern pattern;
    Bytes frame(kI420FrameSize);
    uint8_t frame_num = 0;
    size_t total = 0, idr_size = 0;
    for (int i = 0; i < frames; ++i) {
        pattern.latest(frame.data());
        yuv.write(reinterpret_cast<const char*>(frame.data()), std::streamsize(frame.size()));
        const bool idr = i % 60 == 0;
        const auto f = enc->encode(frame.data(), idr, error);
        if (!f) {
            std::fprintf(stderr, "frame %d: %s\n", i, error.c_str());
            return 1;
        }
        if (f->idr != idr) {
            std::fprintf(stderr, "frame %d: IDR mismatch\n", i);
            return 1;
        }
        frame_num = f->idr ? 0 : uint8_t(frame_num + 1);
        Bytes joined;
        for (const auto& c : f->chunks)
            put_bytes(joined, c);
        const Bytes annexb = reconstruct_annexb(joined, f->idr, frame_num, f->idr);
        h264.write(reinterpret_cast<const char*>(annexb.data()), std::streamsize(annexb.size()));
        total += f->size();
        if (f->idr)
            idr_size = f->size();
    }
    std::printf("encoded %d frames, %zu bytes total (last IDR %zu bytes, average %zu bytes/frame)\n", frames,
                total, idr_size, total / size_t(frames));
    return 0;
}
