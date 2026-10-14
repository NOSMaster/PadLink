// SPDX-License-Identifier: GPL-2.0-or-later
// Debug tool: encodes a raw 864x480 yuv420p file exactly as padlinkd sends it
// and writes the GamePad-style reconstructed .h264 (see video_selftest.cpp).
// usage: padlink-encode-file <in.yuv> <out.h264> [rows|late|even]
#include "video/encoder.h"
#include "video/h264.h"

#include <cstdio>
#include <fstream>
#include <string>

using namespace padlink;

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <in.yuv> <out.h264> [rows|late|even]\n", argv[0]);
        return 2;
    }
    std::string error;
    auto enc = Encoder::create(error);
    if (!enc) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (argc > 3 && std::string(argv[3]) == "even")
        enc->set_chunk_split(ChunkSplit::Even);
    if (argc > 3 && std::string(argv[3]) == "late")
        enc->set_chunk_split(ChunkSplit::Late);
    std::ifstream in(argv[1], std::ios::binary);
    std::ofstream out(argv[2], std::ios::binary);
    Bytes frame(kI420FrameSize);
    uint8_t frame_num = 0;
    for (int i = 0; in.read(reinterpret_cast<char*>(frame.data()), std::streamsize(frame.size())); ++i) {
        const auto f = enc->encode(frame.data(), i == 0, error);
        if (!f) {
            std::fprintf(stderr, "frame %d: %s\n", i, error.c_str());
            return 1;
        }
        frame_num = f->idr ? 0 : uint8_t(frame_num + 1);
        Bytes joined;
        for (const auto& c : f->chunks)
            put_bytes(joined, c);
        if (i < 3)
            std::printf("frame %d: %s %zu bytes, chunks %zu %zu %zu %zu %zu\n", i, f->idr ? "IDR" : "P", f->size(),
                        f->chunks[0].size(), f->chunks[1].size(), f->chunks[2].size(), f->chunks[3].size(),
                        f->chunks[4].size());
        const Bytes annexb = reconstruct_annexb(joined, f->idr, frame_num, f->idr);
        out.write(reinterpret_cast<const char*>(annexb.data()), std::streamsize(annexb.size()));
    }
    return 0;
}
