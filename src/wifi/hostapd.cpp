// SPDX-License-Identifier: GPL-2.0-or-later
#include "wifi/hostapd.h"

#include "util/log.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>

namespace padlink {

namespace {

// A datagram socket connected to hostapd's control socket. Binding with only
// the address family makes Linux pick a unique abstract address for us, so no
// temporary files are left behind.
UniqueFd connect_control(const std::string& path)
{
    UniqueFd fd(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (!fd)
        return {};
    sockaddr_un local{};
    local.sun_family = AF_UNIX;
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&local), sizeof(sa_family_t)) != 0)
        return {};
    sockaddr_un remote{};
    remote.sun_family = AF_UNIX;
    if (path.size() >= sizeof(remote.sun_path))
        return {};
    std::strcpy(remote.sun_path, path.c_str());
    if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != 0)
        return {};
    return fd;
}

std::optional<std::string> request(int fd, const std::string& cmd, int timeout_ms)
{
    if (::send(fd, cmd.data(), cmd.size(), 0) < 0)
        return std::nullopt;
    char buf[4096];
    for (;;) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, timeout_ms) <= 0)
            return std::nullopt;
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            return std::nullopt;
        // Unsolicited events start with '<'; the reply we want does not.
        if (buf[0] != '<')
            return std::string(buf, size_t(n));
    }
}

} // namespace

std::optional<Hostapd> Hostapd::start(const std::string& binary, const std::string& config_path,
                                      const std::string& ctrl_dir, const std::string& iface,
                                      std::string& error)
{
    int pipe_fds[2];
    if (::pipe2(pipe_fds, O_CLOEXEC) != 0) {
        error = std::strerror(errno);
        return std::nullopt;
    }
    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::strerror(errno);
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return std::nullopt;
    }
    if (pid == 0) {
        // Child process: if padlinkd dies, the kernel kills hostapd too.
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() != parent)
            ::_exit(127);
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);  // padlinkd blocks signals; hostapd must not
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        ::dup2(pipe_fds[1], STDERR_FILENO);
        ::execl(binary.c_str(), binary.c_str(), config_path.c_str(), static_cast<char*>(nullptr));
        std::fprintf(stderr, "exec %s failed: %s\n", binary.c_str(), std::strerror(errno));
        ::_exit(127);
    }
    ::close(pipe_fds[1]);

    Hostapd h;
    h.m_pid = pid;
    h.m_output.reset(pipe_fds[0]);
    h.m_ctrl_path = ctrl_dir + "/" + iface;
    return h;
}

Hostapd::Hostapd(Hostapd&& o) noexcept
    : m_pid(std::exchange(o.m_pid, -1)), m_ctrl_path(std::move(o.m_ctrl_path)),
      m_output(std::move(o.m_output)), m_control(std::move(o.m_control)),
      m_events(std::move(o.m_events)), m_partial_line(std::move(o.m_partial_line))
{
}

Hostapd::~Hostapd()
{
    if (m_pid <= 0)
        return;
    ::kill(m_pid, SIGTERM);
    for (int i = 0; i < 200; ++i) {
        if (::waitpid(m_pid, nullptr, WNOHANG) == m_pid)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    log("hostapd did not stop, killing it");
    ::kill(m_pid, SIGKILL);
    ::waitpid(m_pid, nullptr, 0);
}

bool Hostapd::open_control(std::string& error)
{
    m_control = connect_control(m_ctrl_path);
    m_events = connect_control(m_ctrl_path);
    if (!m_control || !m_events) {
        error = "cannot connect to hostapd control socket " + m_ctrl_path;
        return false;
    }
    const auto reply = request(m_events.get(), "ATTACH", 1000);
    if (!reply || reply->rfind("OK", 0) != 0) {
        error = "hostapd refused ATTACH";
        return false;
    }
    return true;
}

bool Hostapd::wait_enabled(int timeout_ms, std::string& error)
{
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    while (clock::now() < deadline) {
        pollfd p{m_output.get(), POLLIN, 0};
        if (::poll(&p, 1, 100) > 0 && !drain_output()) {
            error = "hostapd exited during start-up (see its messages above)";
            return false;
        }
        if (!m_control) {
            std::string ignored;
            if (!open_control(ignored))
                continue;  // control socket not created yet
        }
        const auto status = command("STATUS");
        if (status && status->find("state=ENABLED") != std::string::npos)
            return true;
    }
    error = "timed out waiting for the access point to start";
    return false;
}

std::optional<std::string> Hostapd::command(const std::string& cmd)
{
    if (!m_control)
        return std::nullopt;
    return request(m_control.get(), cmd, 2000);
}

bool Hostapd::drain_output()
{
    char buf[2048];
    const ssize_t n = ::read(m_output.get(), buf, sizeof(buf));
    if (n <= 0)
        return n < 0 && errno == EINTR;
    m_partial_line.append(buf, size_t(n));
    size_t nl;
    while ((nl = m_partial_line.find('\n')) != std::string::npos) {
        log("hostapd: {}", m_partial_line.substr(0, nl));
        m_partial_line.erase(0, nl + 1);
    }
    return true;
}

std::optional<std::string> Hostapd::read_event()
{
    char buf[4096];
    const ssize_t n = ::recv(m_events.get(), buf, sizeof(buf), MSG_DONTWAIT);
    if (n <= 0)
        return std::nullopt;
    std::string ev(buf, size_t(n));
    if (!ev.empty() && ev[0] == '<') {
        const size_t end = ev.find('>');
        if (end != std::string::npos)
            ev.erase(0, end + 1);
    }
    while (!ev.empty() && (ev.back() == '\n' || ev.back() == '\0'))
        ev.pop_back();
    return ev;
}

} // namespace padlink
