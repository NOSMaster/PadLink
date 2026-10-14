// SPDX-License-Identifier: GPL-2.0-or-later
// Sends 48 kHz stereo PCM to the GamePad every 8.667 ms on its own thread.
#pragma once

#include "audio/audio_source.h"
#include "clock/tsf_clock.h"
#include "net/drc_sockets.h"

#include <atomic>
#include <memory>
#include <thread>

namespace padlink {

class AudioSender {
public:
    AudioSender(DrcSockets& sockets, TsfClock& clock, std::unique_ptr<AudioSource> source);
    ~AudioSender();

    void set_active(bool active) { m_active = active; }
    void set_rumble(bool on) { m_rumble = on; }  // sets the vibrate bit on outgoing packets

    uint64_t packets() const { return m_packets; }
    uint64_t silent_packets() const { return m_silent; }
    uint64_t skipped_clock() const { return m_skipped_clock; }

private:
    void run();
    DrcSockets& m_sockets;
    TsfClock& m_clock;
    std::unique_ptr<AudioSource> m_source;
    std::atomic<bool> m_active{false}, m_rumble{false}, m_stop{false};
    std::atomic<uint64_t> m_packets{0}, m_silent{0}, m_skipped_clock{0};
    std::thread m_thread;
};

} // namespace padlink
