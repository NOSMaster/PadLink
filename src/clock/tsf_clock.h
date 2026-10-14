// SPDX-License-Identifier: GPL-2.0-or-later
// MediaClock based on the access point's TSF (PROTOCOL.md §7).
//
// The hardware TSF is sampled now and then (each read takes a few debugfs
// system calls). Between samples we add the elapsed CLOCK_MONOTONIC time.
// A sample that disagrees with that prediction marks the clock unhealthy, and
// media must not be sent while it is.
//
// Thread-safe: the main loop samples it while the video thread reads it.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace padlink {

class TsfClock {
public:
    using Reader = std::function<std::optional<uint64_t>()>;  // returns TSF in µs
    using Now = std::function<int64_t()>;                     // monotonic µs

    struct Limits {
        int64_t max_read_us = 2000;      // a slower read is too imprecise
        int64_t max_jump_us = 5000;      // allowed disagreement with prediction
        int64_t max_age_us = 1000000;    // samples older than this: unhealthy
    };

    TsfClock(Reader reader, Now now, Limits limits);
    TsfClock(Reader reader, Now now) : TsfClock(std::move(reader), std::move(now), Limits{}) {}

    // Takes one sample. Returns false (and marks the clock unhealthy) if it
    // failed or is inconsistent.
    bool sample();

    bool healthy() const;
    uint64_t now_tsf() const;             // extrapolated 64-bit TSF
    uint32_t timestamp() const { return uint32_t(now_tsf()); }  // media timestamp

    // TSF rate relative to CLOCK_MONOTONIC since the first good sample, in
    // parts per million. Only meaningful after several seconds.
    std::optional<double> drift_ppm() const;
    std::string last_error() const;

private:
    mutable std::mutex m_mutex;
    Reader m_reader;
    Now m_now;
    Limits m_limits;
    bool m_have_anchor = false;
    bool m_healthy = false;
    uint64_t m_tsf = 0;          // last good sample
    int64_t m_mono = 0;          // monotonic time of that sample
    uint64_t m_first_tsf = 0;
    int64_t m_first_mono = 0;
    std::string m_error;
};

int64_t monotonic_us();

// CLOCK_MONOTONIC_RAW in µs. Unlike CLOCK_MONOTONIC, NTP never speeds it up
// or slows it down (by up to 500 ppm), so it is the right base for
// extrapolating the TSF between samples. Use monotonic_us() for sleeping.
int64_t monotonic_raw_us();

// Reads the RTL8852BE AP-port TSF (registers 0xC43C high / 0xC438 low) through
// rtw89 debugfs. Read-only; the high word is read twice to detect a carry.
// `read_reg_path` is /sys/kernel/debug/ieee80211/<phy>/rtw89/read_reg.
std::optional<uint64_t> rtw89_read_ap_tsf(const std::string& read_reg_path);

// Parses rtw89's read_reg output, e.g. "get 4 bytes at 0x0000c438=0x1234abcd".
std::optional<uint32_t> parse_rtw89_read_reg(const std::string& text, uint32_t expected_addr);

// Finds the read_reg file for an interface, if rtw89 debugfs is available.
std::optional<std::string> rtw89_read_reg_path(const std::string& iface);

} // namespace padlink
