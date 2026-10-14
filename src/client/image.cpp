// SPDX-License-Identifier: GPL-2.0-or-later
// RGBA (any size) -> the GamePad's 864x480 yuv420p frame.
#include "padlink/padlink.h"

#include <algorithm>
#include <cstring>

namespace padlink {

namespace {

constexpr int kCodedWidth = 864;  // 854 visible + 10 hidden columns

// BT.601 limited range, fixed point (what H.264 decoders assume by default).
inline uint8_t luma(int r, int g, int b) { return uint8_t(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16); }
inline uint8_t cb(int r, int g, int b) { return uint8_t(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128); }
inline uint8_t cr(int r, int g, int b) { return uint8_t(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128); }

} // namespace

void rgba_to_gamepad_i420(const uint8_t* rgba, int width, int height, int stride, uint8_t* out)
{
    uint8_t* Y = out;
    uint8_t* U = Y + kCodedWidth * kScreenHeight;
    uint8_t* V = U + kCodedWidth * kScreenHeight / 4;
    std::memset(Y, 16, size_t(kCodedWidth) * kScreenHeight);  // black
    std::memset(U, 128, size_t(kCodedWidth) * kScreenHeight / 4);
    std::memset(V, 128, size_t(kCodedWidth) * kScreenHeight / 4);
    if (!rgba || width <= 0 || height <= 0)
        return;

    // Fit the image inside 854x480, keeping its aspect ratio (black bars).
    int w = kScreenWidth, h = kScreenHeight;
    if (int64_t(width) * kScreenHeight > int64_t(height) * kScreenWidth)
        h = std::max(2, int(int64_t(height) * kScreenWidth / width) & ~1);
    else
        w = std::max(2, int(int64_t(width) * kScreenHeight / height) & ~1);
    const int x0 = ((kScreenWidth - w) / 2) & ~1, y0 = ((kScreenHeight - h) / 2) & ~1;

    // Nearest-neighbour sampling: simple and fast enough for 854x480 at 60 fps.
    auto pixel = [&](int x, int y) -> const uint8_t* {
        const int sx = std::min(width - 1, int(int64_t(x) * width / w));
        const int sy = std::min(height - 1, int(int64_t(y) * height / h));
        return rgba + size_t(sy) * size_t(stride) + size_t(sx) * 4;
    };
    for (int y = 0; y < h; ++y) {
        uint8_t* row = Y + size_t(y0 + y) * kCodedWidth + x0;
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = pixel(x, y);
            row[x] = luma(p[0], p[1], p[2]);
        }
    }
    // Chroma: one sample per 2x2 block, averaged.
    for (int y = 0; y < h / 2; ++y) {
        for (int x = 0; x < w / 2; ++x) {
            int r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const uint8_t* p = pixel(2 * x + dx, 2 * y + dy);
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
            const size_t i = size_t(y0 / 2 + y) * (kCodedWidth / 2) + size_t(x0 / 2 + x);
            U[i] = cb(r / 4, g / 4, b / 4);
            V[i] = cr(r / 4, g / 4, b / 4);
        }
    }
}

} // namespace padlink
