// SPDX-License-Identifier: GPL-2.0-or-later
// Where video frames come from: a built-in test pattern, or raw I420 frames
// piped in by another program (e.g. a screen recorder running as the desktop user).
#pragma once

#include "video/encoder.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace padlink {

class FrameSource {
public:
    virtual ~FrameSource() = default;
    // Fills `out` (kI420FrameSize bytes) with the newest frame. Returns false
    // if no frame is available yet.
    virtual bool latest(uint8_t* out) = 0;
};

// Moving color bars, a bouncing box and a frame counter: easy to judge for
// smoothness and corruption by eye.
class TestPattern : public FrameSource {
public:
    bool latest(uint8_t* out) override;

private:
    uint32_t m_frame = 0;
};

// Reads 864x480 yuv420p frames from a file descriptor (stdin or a FIFO) on a
// background thread and always hands out the most recent complete frame.
class PipeSource : public FrameSource {
public:
    explicit PipeSource(int fd);
    ~PipeSource() override;
    bool latest(uint8_t* out) override;

private:
    void run();
    int m_fd;
    std::mutex m_mutex;
    Bytes m_frame;
    bool m_have_frame = false;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

} // namespace padlink
