// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/audio_source.h"

#include "util/log.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

namespace padlink {

size_t ToneSource::read(int16_t* out, size_t frames)
{
    constexpr double kTwoPi = 6.283185307179586;
    for (size_t i = 0; i < frames; ++i, ++m_pos) {
        const uint64_t in_second = m_pos % kAudioRate;
        const bool on = in_second < kAudioRate / 5;  // 200 ms beep, then silence
        const double t = double(m_pos) / kAudioRate;
        out[2 * i] = on ? int16_t(6000 * std::sin(kTwoPi * 440 * t)) : 0;
        out[2 * i + 1] = on ? int16_t(6000 * std::sin(kTwoPi * 660 * t)) : 0;
    }
    return frames;
}

size_t ring_push(std::vector<int16_t>& ring, size_t& head, size_t& count, const int16_t* frames, size_t n,
                 size_t max_frames, size_t trim_frames)
{
    const size_t capacity = ring.size() / 2;
    size_t dropped = 0;
    for (size_t i = 0; i < n; ++i) {
        if (count == capacity) {  // full: overwrite the oldest frame
            head = (head + 1) % capacity;
            --count;
            ++dropped;
        }
        const size_t tail = (head + count) % capacity;
        ring[2 * tail] = frames[2 * i];
        ring[2 * tail + 1] = frames[2 * i + 1];
        ++count;
    }
    if (count > max_frames) {
        const size_t drop = count - trim_frames;
        head = (head + drop) % capacity;
        count -= drop;
        dropped += drop;
    }
    return dropped;
}

size_t ring_pop(std::vector<int16_t>& ring, size_t& head, size_t& count, int16_t* out, size_t n)
{
    const size_t capacity = ring.size() / 2;
    const size_t got = std::min(n, count);
    for (size_t i = 0; i < got; ++i) {
        out[2 * i] = ring[2 * head];
        out[2 * i + 1] = ring[2 * head + 1];
        head = (head + 1) % capacity;
    }
    count -= got;
    std::fill(out + 2 * got, out + 2 * n, int16_t(0));  // silence for the rest
    return got;
}

FifoSource::FifoSource(std::string path) : m_path(std::move(path)), m_ring(2 * kAudioRate, 0)
{
    // 0666: the desktop user writes to it while padlinkd runs as root.
    struct stat st{};
    if (::stat(m_path.c_str(), &st) != 0) {
        const mode_t old = ::umask(0);
        if (::mkfifo(m_path.c_str(), 0666) != 0)
            log("audio: cannot create FIFO {}: {}", m_path, std::strerror(errno));
        ::umask(old);
    } else if (!S_ISFIFO(st.st_mode)) {
        log("audio: {} exists and is not a FIFO", m_path);
    }
    m_thread = std::thread([this] { run(); });
}

FifoSource::~FifoSource()
{
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join();
}

void FifoSource::run()
{
    // O_RDWR keeps the FIFO open even while no writer is connected, so the
    // writer can start and stop at any time without us seeing end-of-file.
    const int fd = ::open(m_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        log("audio: cannot open {}: {}", m_path, std::strerror(errno));
        return;
    }
    std::vector<uint8_t> buf(4096);
    size_t carry = 0;  // bytes of an incomplete frame kept for the next read
    bool announced = false;
    while (!m_stop) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 200) <= 0)
            continue;
        const ssize_t n = ::read(fd, buf.data() + carry, buf.size() - carry);
        if (n <= 0)
            continue;
        const size_t total = carry + size_t(n);
        const size_t frames = total / 4;
        if (frames > 0) {
            std::vector<int16_t> samples(frames * 2);
            for (size_t i = 0; i < frames * 2; ++i)
                samples[i] = int16_t(buf[2 * i] | buf[2 * i + 1] << 8);  // s16 little-endian
            std::lock_guard lock(m_mutex);
            m_dropped += ring_push(m_ring, m_head, m_count, samples.data(), frames, kMaxBufferedFrames, kTrimToFrames);
        }
        carry = total - frames * 4;
        std::memmove(buf.data(), buf.data() + frames * 4, carry);
        if (!announced) {
            log("audio: receiving PCM from {}", m_path);
            announced = true;
        }
    }
    ::close(fd);
}

size_t FifoSource::read(int16_t* out, size_t frames)
{
    std::lock_guard lock(m_mutex);
    const size_t got = ring_pop(m_ring, m_head, m_count, out, frames);
    m_underrun += frames - got;
    return got;
}

} // namespace padlink
