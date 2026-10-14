// SPDX-License-Identifier: GPL-2.0-or-later
#include "clock/tsf_clock.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace padlink {

TsfClock::TsfClock(Reader reader, Now now, Limits limits)
    : m_reader(std::move(reader)), m_now(std::move(now)), m_limits(limits)
{
}

bool TsfClock::sample()
{
    const int64_t before = m_now();
    const auto tsf = m_reader();  // slow part: done without holding the lock
    const int64_t after = m_now();
    std::lock_guard lock(m_mutex);
    const int64_t mid = before + (after - before) / 2;

    if (!tsf) {
        m_error = "TSF read failed";
        m_healthy = false;
        return false;
    }
    if (after - before > m_limits.max_read_us) {
        m_error = "TSF read took " + std::to_string(after - before) + " us";
        m_healthy = false;
        return false;  // keep the previous anchor; just skip this sample
    }
    if (m_have_anchor) {
        const int64_t predicted = int64_t(m_tsf) + (mid - m_mono);
        const int64_t error = int64_t(*tsf) - predicted;
        if (error > m_limits.max_jump_us || error < -m_limits.max_jump_us) {
            m_error = "TSF jumped by " + std::to_string(error) + " us";
            // Restart from this sample. Stay unhealthy until a following
            // sample agrees with it.
            m_tsf = *tsf;
            m_mono = mid;
            m_first_tsf = *tsf;
            m_first_mono = mid;
            m_healthy = false;
            return false;
        }
    } else {
        m_first_tsf = *tsf;
        m_first_mono = mid;
    }
    const bool confirmed = m_have_anchor;  // need two consistent samples
    m_tsf = *tsf;
    m_mono = mid;
    m_have_anchor = true;
    m_healthy = confirmed;
    m_error.clear();
    return true;
}

bool TsfClock::healthy() const
{
    std::lock_guard lock(m_mutex);
    return m_healthy && m_now() - m_mono <= m_limits.max_age_us;
}

uint64_t TsfClock::now_tsf() const
{
    std::lock_guard lock(m_mutex);
    return m_tsf + uint64_t(m_now() - m_mono);
}

std::string TsfClock::last_error() const
{
    std::lock_guard lock(m_mutex);
    return m_error;
}

std::optional<double> TsfClock::drift_ppm() const
{
    std::lock_guard lock(m_mutex);
    const int64_t mono = m_mono - m_first_mono;
    if (!m_have_anchor || mono <= 0)
        return std::nullopt;
    const int64_t tsf = int64_t(m_tsf - m_first_tsf);
    return double(tsf - mono) * 1e6 / double(mono);
}

int64_t monotonic_us()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

int64_t monotonic_raw_us()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return int64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

std::optional<uint32_t> parse_rtw89_read_reg(const std::string& text, uint32_t expected_addr)
{
    unsigned bytes = 0, addr = 0, value = 0;
    if (std::sscanf(text.c_str(), "get %u bytes at 0x%x=0x%x", &bytes, &addr, &value) != 3)
        return std::nullopt;
    if (bytes != 4 || addr != expected_addr || value == 0xdeadbeef)
        return std::nullopt;
    return value;
}

namespace {

std::optional<uint32_t> read_reg(const std::string& path, uint32_t addr)
{
    {
        // Select the register: "<hex address> <length>".
        std::ofstream sel(path);
        char cmd[32];
        std::snprintf(cmd, sizeof(cmd), "%x 4\n", addr);
        sel << cmd;
        if (!sel.flush())
            return std::nullopt;
    }
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line))
        return std::nullopt;
    return parse_rtw89_read_reg(line, addr);
}

constexpr uint32_t kTsfLow = 0xc438;
constexpr uint32_t kTsfHigh = 0xc43c;

} // namespace

std::optional<uint64_t> rtw89_read_ap_tsf(const std::string& path)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto hi1 = read_reg(path, kTsfHigh);
        const auto lo = read_reg(path, kTsfLow);
        const auto hi2 = read_reg(path, kTsfHigh);
        if (!hi1 || !lo || !hi2)
            return std::nullopt;
        if (*hi1 == *hi2)  // no carry from low into high word in between
            return uint64_t(*hi1) << 32 | *lo;
    }
    return std::nullopt;
}

std::optional<std::string> rtw89_read_reg_path(const std::string& iface)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dev = fs::path("/sys/class/net") / iface;
    const auto driver = fs::canonical(dev / "device/driver", ec);
    if (ec || driver.filename().string().rfind("rtw89", 0) != 0)
        return std::nullopt;
    const auto phy = fs::canonical(dev / "phy80211", ec);
    if (ec)
        return std::nullopt;
    const fs::path path = fs::path("/sys/kernel/debug/ieee80211") / phy.filename() / "rtw89/read_reg";
    if (!fs::exists(path, ec))
        return std::nullopt;
    return path.string();
}

} // namespace padlink
