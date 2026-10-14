// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio/audio_sender.h"

#include "util/log.h"

#include <cerrno>
#include <ctime>

namespace padlink {

namespace {

void sleep_until(int64_t when_us)
{
    timespec ts{};
    ts.tv_sec = when_us / 1000000;
    ts.tv_nsec = (when_us % 1000000) * 1000;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
}

} // namespace

AudioSender::AudioSender(DrcSockets& sockets, TsfClock& clock, std::unique_ptr<AudioSource> source)
    : m_sockets(sockets), m_clock(clock), m_source(std::move(source))
{
    m_thread = std::thread([this] { run(); });
}

AudioSender::~AudioSender()
{
    m_stop = true;
    if (m_thread.joinable())
        m_thread.join();
}

void AudioSender::run()
{
    int16_t pcm[kAudioFramesPerPacket * kAudioChannels];
    uint16_t sequence = 0;
    int64_t start = 0;
    uint64_t index = 0;  // packets since start; computing each deadline from
                         // `start` avoids accumulating rounding errors
    while (!m_stop) {
        if (!m_active) {
            start = 0;
            m_source->read(pcm, kAudioFramesPerPacket);  // keep the source drained
            sleep_until(monotonic_us() + int64_t(kAudioFramesPerPacket) * 1000000 / kAudioRate);
            continue;
        }
        if (start == 0) {
            start = monotonic_us();
            index = 0;
            log("audio: streaming started");
        }
        // Packet n is due at start + n * 416 / 48000 s (every 8.667 ms).
        const int64_t due = start + int64_t(index * kAudioFramesPerPacket * 1000000 / kAudioRate);
        sleep_until(due);
        ++index;
        if (monotonic_us() - due > 100000) {  // stalled for >100 ms: restart the schedule
            start = 0;
            continue;
        }

        const size_t real = m_source->read(pcm, kAudioFramesPerPacket);
        if (!m_clock.healthy()) {
            ++m_skipped_clock;
            continue;
        }
        if (real == 0)
            ++m_silent;
        AstrmAudioHeader h;
        h.sequence = sequence;
        h.vibrate = m_rumble;
        h.timestamp = m_clock.timestamp();
        sequence = (sequence + 1) & 0x3ff;
        m_sockets.send(DrcPort::Audio,
                       build_audio_packet(h, ByteView(reinterpret_cast<const uint8_t*>(pcm), sizeof(pcm))));
        ++m_packets;
    }
}

} // namespace padlink
