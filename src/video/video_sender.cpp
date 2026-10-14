// SPDX-License-Identifier: GPL-2.0-or-later
#include "video/video_sender.h"

#include "protocol/vstrm.h"
#include "util/log.h"

#include <algorithm>
#include <ctime>

namespace padlink {

namespace {

constexpr int64_t kFramePeriodUs = 1001000 / 60;  // 16683 µs (59.94 Hz)
constexpr int64_t kFormatLeadUs = 5000;           // format message -> first video packet
constexpr int64_t kSendWindowUs = 11000;          // spread a frame's packets over this
constexpr uint32_t kTimestampLeadUs = 1250;       // format stamped 1.25 ms in the past
// The GamePad repeats its "send an IDR" message until one arrives. Requests
// within this time after we sent an IDR were most likely sent before it
// arrived, so they are not answered with yet another IDR.
constexpr int64_t kIdrRequestHoldoffUs = 3 * kFramePeriodUs;

// Sleeps until the given CLOCK_MONOTONIC time (µs).
void sleep_until(int64_t when_us)
{
    timespec ts{};
    ts.tv_sec = when_us / 1000000;
    ts.tv_nsec = (when_us % 1000000) * 1000;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
}

} // namespace

VideoSender::VideoSender(DrcSockets& sockets, TsfClock& clock, std::unique_ptr<FrameSource> source,
                         std::unique_ptr<Encoder> encoder, Options options)
    : m_sockets(sockets), m_clock(clock), m_source(std::move(source)), m_encoder(std::move(encoder)),
      m_options(options)
{
    m_thread = std::thread([this] { run(); });
}

VideoSender::~VideoSender()
{
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join();
}

void VideoSender::run()
{
    // Start with a black picture (Y=16, U=V=128) so the GamePad gets video
    // even before the source delivers its first frame. Without any video it
    // gives up after ~20 s ("connection failed").
    Bytes frame(kI420FrameSize, 128);
    std::fill(frame.begin(), frame.begin() + kVideoWidth * kVideoHeight, uint8_t(16));
    bool streaming = false, format_only_next = false, logged_waiting = false;
    int frames_since_idr = 0;
    int64_t last_idr_sent = 0;
    uint16_t sequence = 0;
    int64_t slot = monotonic_us();

    while (!m_stop) {
        if (!m_active) {
            if (streaming)
                log("video: paused");
            streaming = false;
            sleep_until(monotonic_us() + 20000);
            slot = monotonic_us();
            continue;
        }
        sleep_until(slot);
        const int64_t slot_start = slot;
        slot += kFramePeriodUs;
        if (monotonic_us() - slot_start > kFramePeriodUs) {
            slot = monotonic_us() + kFramePeriodUs;  // fell behind: resynchronize
        }
        if (!m_clock.healthy()) {
            ++m_stats.skipped_clock;  // never send media with a wrong timestamp
            continue;
        }

        const uint32_t ts = m_clock.timestamp() - kTimestampLeadUs;
        m_sockets.send(DrcPort::Audio, build_video_format(ts));
        if (format_only_next) {
            // After an IDR, a real console leaves one slot with only the format message.
            format_only_next = false;
            continue;
        }

        if (!m_source->latest(frame.data()) && !logged_waiting) {
            log("video: no frames from the source yet; sending black until they arrive");
            logged_waiting = true;
        }

        const bool init = !streaming;
        bool requested = m_idr_requested.exchange(false);
        if (requested && slot_start - last_idr_sent < kIdrRequestHoldoffUs) {
            ++m_stats.idr_requests_ignored;
            requested = false;
        }
        const bool idr = init || requested ||
                         (m_options.periodic_idr_frames > 0 && frames_since_idr >= m_options.periodic_idr_frames);
        const int64_t t0 = monotonic_us();
        std::string error;
        const auto encoded = m_encoder->encode(frame.data(), idr, error);
        const int64_t encode_us = monotonic_us() - t0;
        if (encode_us > m_stats.max_encode_us)
            m_stats.max_encode_us = encode_us;
        if (!encoded) {
            ++m_stats.errors;
            log("video: encode failed: {}", error);
            continue;
        }
        if (init) {
            log("video: streaming started (first frame IDR, {} bytes)", encoded->size());
            streaming = true;
        }
        frames_since_idr = encoded->idr ? 0 : frames_since_idr + 1;

        const bool init_flag = init || (encoded->idr && m_options.init_on_every_idr);
        const auto packets = packetize_frame(encoded->chunks, encoded->idr, init_flag, ts, sequence);
        const int64_t first = slot_start + kFormatLeadUs;
        for (size_t i = 0; i < packets.size(); ++i) {
            sleep_until(first + int64_t(i) * kSendWindowUs / int64_t(packets.size()));
            if (!m_sockets.send(DrcPort::Video, packets[i]))
                ++m_stats.errors;
            m_stats.bytes += packets[i].size();
        }
        m_stats.packets += packets.size();
        ++m_stats.frames;
        if (encoded->idr) {
            ++m_stats.idr_frames;
            last_idr_sent = slot_start;
            m_last_idr_us = slot_start;
            m_frames_since_idr = 0;
            // A real console leaves one format-only slot after an IDR; without
            // it the GamePad never displays anything (tested on hardware).
            format_only_next = true;
        } else {
            ++m_frames_since_idr;
        }
    }
}

} // namespace padlink
