// SPDX-License-Identifier: GPL-2.0-or-later
// Where GamePad audio comes from: a test tone, or 48 kHz stereo s16le PCM
// written into a FIFO by a program running as the desktop user (see
// scripts/padlink-audio.sh, which records a "Wii U GamePad" PipeWire sink).
#pragma once

#include "protocol/astrm.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace padlink {

class AudioSource {
public:
    virtual ~AudioSource() = default;
    // Fills `out` with `frames` stereo frames (2 * frames samples). Missing
    // data is filled with silence. Returns how many frames were real audio.
    virtual size_t read(int16_t* out, size_t frames) = 0;
};

// Silence (used when only rumble is wanted).
class SilenceSource : public AudioSource {
public:
    size_t read(int16_t* out, size_t frames) override
    {
        std::fill(out, out + 2 * frames, int16_t(0));
        return 0;
    }
};

// A short beep every second (440 Hz left, 660 Hz right) at a modest volume.
class ToneSource : public AudioSource {
public:
    size_t read(int16_t* out, size_t frames) override;

private:
    uint64_t m_pos = 0;
};

// Reads raw PCM from a named pipe (FIFO) on a background thread into a ring
// buffer. Keeps latency bounded: if the writer runs ahead (its clock is a bit
// faster than ours), the oldest audio is dropped.
class FifoSource : public AudioSource {
public:
    explicit FifoSource(std::string path);  // creates the FIFO (mode 0666) if needed
    ~FifoSource() override;
    size_t read(int16_t* out, size_t frames) override;

    uint64_t dropped_frames() const { return m_dropped; }
    uint64_t underrun_frames() const { return m_underrun; }

    static constexpr size_t kMaxBufferedFrames = 4800;  // 100 ms
    static constexpr size_t kTrimToFrames = 1920;       // 40 ms

private:
    void run();
    std::string m_path;
    std::mutex m_mutex;
    std::vector<int16_t> m_ring;  // interleaved stereo
    size_t m_head = 0, m_count = 0;  // in frames
    std::atomic<uint64_t> m_dropped{0}, m_underrun{0};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

// Pure helper (unit tested): appends frames to a ring buffer of `capacity`
// frames; when more than `max_frames` would be buffered, drops oldest frames
// down to `trim_frames`. Returns the number of frames dropped.
size_t ring_push(std::vector<int16_t>& ring, size_t& head, size_t& count, const int16_t* frames, size_t n,
                 size_t max_frames, size_t trim_frames);
size_t ring_pop(std::vector<int16_t>& ring, size_t& head, size_t& count, int16_t* out, size_t n);

} // namespace padlink
