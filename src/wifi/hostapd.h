// SPDX-License-Identifier: GPL-2.0-or-later
// Runs our patched hostapd as a child process and talks to it through its
// control socket (the same interface hostapd_cli uses).
#pragma once

#include "util/unique_fd.h"

#include <optional>
#include <string>
#include <sys/types.h>

namespace padlink {

class Hostapd {
public:
    // Starts `binary config_path`. hostapd's own output is forwarded to our
    // log by drain_output(). Returns std::nullopt and fills `error` on failure.
    static std::optional<Hostapd> start(const std::string& binary, const std::string& config_path,
                                        const std::string& ctrl_dir, const std::string& iface,
                                        std::string& error);

    Hostapd(Hostapd&&) noexcept;
    Hostapd& operator=(Hostapd&&) = delete;
    ~Hostapd();  // stops the process (SIGTERM, then SIGKILL after 2 s)

    // Waits until hostapd reports state=ENABLED (the AP is beaconing).
    bool wait_enabled(int timeout_ms, std::string& error);

    // Sends one control command (e.g. "STATUS") and returns the reply.
    std::optional<std::string> command(const std::string& cmd);

    // Descriptors to poll(): hostapd's stdout/stderr, and attached events.
    int output_fd() const { return m_output.get(); }
    int event_fd() const { return m_events.get(); }

    // Forwards available output lines to the log. Returns false at EOF,
    // which means hostapd has exited.
    bool drain_output();

    // Reads one pending event such as "<3>AP-STA-CONNECTED aa:bb:..".
    // The "<n>" priority prefix is removed.
    std::optional<std::string> read_event();

private:
    Hostapd() = default;
    bool open_control(std::string& error);

    pid_t m_pid = -1;
    std::string m_ctrl_path;
    UniqueFd m_output;
    UniqueFd m_control;  // request/response socket
    UniqueFd m_events;   // socket that sent ATTACH
    std::string m_partial_line;
};

} // namespace padlink
