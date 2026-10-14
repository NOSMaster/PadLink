// SPDX-License-Identifier: GPL-2.0-or-later
// Sends video to the GamePad on its own thread, paced like a real console:
// every 1/59.94 s a video-format message, then ~5 ms later the frame's
// packets spread over ~11 ms (PROTOCOL.md §4.4).
#pragma once

#include "clock/tsf_clock.h"
#include "net/drc_sockets.h"
#include "video/encoder.h"
#include "video/frame_source.h"

#include <atomic>
#include <memory>
#include <thread>

namespace padlink {

struct VideoStats {
    std::atomic<uint64_t> frames{0}, idr_frames{0}, packets{0}, bytes{0}, skipped_clock{0}, errors{0};
    std::atomic<uint64_t> idr_requests_ignored{0};  // duplicates sent before our IDR arrived
    std::atomic<int64_t> max_encode_us{0};
};

class VideoSender {
public:
    struct Options {
        int periodic_idr_frames = 0;   // 0 = IDRs only at start and on request
        bool answer_idr_requests = true;
        bool init_on_every_idr = true;  // VSTRM init flag on every IDR; looked best on hardware
    };
    VideoSender(DrcSockets& sockets, TsfClock& clock, std::unique_ptr<FrameSource> source,
                std::unique_ptr<Encoder> encoder, Options options);
    ~VideoSender();  // stops the thread

    // Streams only while active (GamePad connected). Each activation starts
    // with an IDR frame carrying the init flag.
    void set_active(bool active) { m_active = active; }
    void request_idr() { m_idr_requested = m_options.answer_idr_requests; }  // GamePad asked for one (MSG)
    const VideoStats& stats() const { return m_stats; }

    // For diagnostics: when the last IDR went out, and frames sent since.
    int64_t last_idr_us() const { return m_last_idr_us; }
    uint64_t frames_since_idr() const { return m_frames_since_idr; }

private:
    void run();

    DrcSockets& m_sockets;
    TsfClock& m_clock;
    std::unique_ptr<FrameSource> m_source;
    std::unique_ptr<Encoder> m_encoder;
    Options m_options;
    std::atomic<int64_t> m_last_idr_us{0};
    std::atomic<uint64_t> m_frames_since_idr{0};
    std::atomic<bool> m_active{false}, m_idr_requested{false}, m_stop{false};
    VideoStats m_stats;
    std::thread m_thread;
};

} // namespace padlink
