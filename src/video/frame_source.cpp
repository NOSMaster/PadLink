// SPDX-License-Identifier: GPL-2.0-or-later
#include "video/frame_source.h"

#include "util/log.h"

#include <algorithm>
#include <cstring>
#include <poll.h>
#include <unistd.h>

namespace padlink {

namespace {

struct Yuv {
    uint8_t y, u, v;
};

// BT.601 colors for the bars: white, yellow, cyan, green, magenta, red, blue, black.
constexpr Yuv kBars[8] = {{235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
                          {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};

void fill_rect(uint8_t* f, int x0, int y0, int w, int h, Yuv c)
{
    uint8_t* yp = f;
    uint8_t* up = f + kVideoWidth * kVideoHeight;
    uint8_t* vp = up + kVideoWidth * kVideoHeight / 4;
    const int x1 = std::min(x0 + w, kVideoWidth), y1 = std::min(y0 + h, kVideoHeight);
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    for (int y = y0; y < y1; ++y)
        std::memset(yp + y * kVideoWidth + x0, c.y, size_t(std::max(0, x1 - x0)));
    for (int y = y0 / 2; y < (y1 + 1) / 2; ++y) {
        std::memset(up + y * (kVideoWidth / 2) + x0 / 2, c.u, size_t(std::max(0, (x1 + 1) / 2 - x0 / 2)));
        std::memset(vp + y * (kVideoWidth / 2) + x0 / 2, c.v, size_t(std::max(0, (x1 + 1) / 2 - x0 / 2)));
    }
}

// A digit as a 3x5 block font, one bit per cell, top row first.
constexpr uint16_t kDigits[10] = {0x7b6f, 0x2492, 0x73e7, 0x73cf, 0x5bc9, 0x79cf, 0x79ef, 0x7249, 0x7bef, 0x7bcf};

void draw_number(uint8_t* f, int x, int y, uint32_t n, int cell)
{
    char text[12];
    const int len = std::snprintf(text, sizeof(text), "%u", n);
    for (int i = 0; i < len; ++i) {
        const uint16_t bits = kDigits[text[i] - '0'];
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < 3; ++c)
                if (bits >> (14 - (r * 3 + c)) & 1)
                    fill_rect(f, x + (i * 4 + c) * cell, y + r * cell, cell, cell, {235, 128, 128});
    }
}

} // namespace

bool TestPattern::latest(uint8_t* f)
{
    const uint32_t n = m_frame++;
    // Top 2/3: color bars scrolling slowly sideways.
    const int bar_w = 854 / 8 + 1;
    const int shift = int(n * 2 % 854);
    fill_rect(f, 0, 0, kVideoWidth, kVideoHeight, {16, 128, 128});
    for (int i = 0; i < 9; ++i) {
        const int x = i * bar_w - shift % bar_w;
        fill_rect(f, x, 0, bar_w, 320, kBars[size_t(i + shift / bar_w) % 8]);
    }
    // Bottom: a gray ramp, a bouncing box and the frame number.
    for (int i = 0; i < 16; ++i)
        fill_rect(f, i * 54, 320, 54, 40, {uint8_t(16 + i * 14), 128, 128});
    const int period = 2 * (854 - 60);
    const int pos = int(n * 6 % uint32_t(period));
    fill_rect(f, pos < 854 - 60 ? pos : period - pos, 370, 60, 60, {81, 90, 240});
    draw_number(f, 20, 440, n, 6);
    return true;
}

PipeSource::PipeSource(int fd) : m_fd(fd), m_frame(kI420FrameSize)
{
    m_thread = std::thread([this] { run(); });
}

PipeSource::~PipeSource()
{
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join();
}

void PipeSource::run()
{
    Bytes buf(kI420FrameSize);
    size_t have = 0;
    uint64_t frames = 0;
    while (!m_stop) {
        pollfd p{m_fd, POLLIN, 0};
        if (::poll(&p, 1, 200) <= 0)
            continue;
        const ssize_t n = ::read(m_fd, buf.data() + have, buf.size() - have);
        if (n <= 0) {
            log("video: input pipe closed after {} frames", frames);
            return;
        }
        have += size_t(n);
        if (have == buf.size()) {
            std::lock_guard lock(m_mutex);
            m_frame.swap(buf);
            m_have_frame = true;
            have = 0;
            if (frames++ == 0)
                log("video: receiving frames from the input pipe");
        }
    }
}

bool PipeSource::latest(uint8_t* out)
{
    std::lock_guard lock(m_mutex);
    if (!m_have_frame)
        return false;
    std::memcpy(out, m_frame.data(), kI420FrameSize);
    return true;
}

} // namespace padlink
