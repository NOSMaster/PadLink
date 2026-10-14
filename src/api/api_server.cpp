// SPDX-License-Identifier: GPL-2.0-or-later
#include "api/api_server.h"

#include "api/wire.h"
#include "clock/tsf_clock.h"  // monotonic_us()
#include "util/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace padlink {

// ---------------------------------------------------------------- sources

bool ApiVideoSource::latest(uint8_t* out)
{
    {
        std::lock_guard lock(m_mutex);
        if (app_active(monotonic_us()) && m_frame.size() == wire::kI420Size) {
            std::memcpy(out, m_frame.data(), m_frame.size());
            return true;
        }
    }
    return m_fallback ? m_fallback->latest(out) : false;
}

void ApiVideoSource::submit(ByteView i420, int64_t now_us)
{
    std::lock_guard lock(m_mutex);
    m_frame.assign(i420.begin(), i420.end());
    if (m_last_frame_us < now_us - 1000000)
        log("api: an app is now sending video");
    m_last_frame_us = now_us;
}

ApiAudioSource::ApiAudioSource(std::unique_ptr<AudioSource> fallback)
    : m_fallback(std::move(fallback)), m_ring(2 * kAudioRate, 0)
{
}

size_t ApiAudioSource::read(int16_t* out, size_t frames)
{
    {
        std::lock_guard lock(m_mutex);
        if (monotonic_us() - m_last_audio_us < 500000)
            return ring_pop(m_ring, m_head, m_count, out, frames);
    }
    if (m_fallback)
        return m_fallback->read(out, frames);
    std::fill(out, out + 2 * frames, int16_t(0));
    return 0;
}

void ApiAudioSource::submit(ByteView pcm, int64_t now_us)
{
    const size_t frames = pcm.size() / 4;
    std::vector<int16_t> samples(frames * 2);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = int16_t(pcm[2 * i] | pcm[2 * i + 1] << 8);
    std::lock_guard lock(m_mutex);
    ring_push(m_ring, m_head, m_count, samples.data(), frames, FifoSource::kMaxBufferedFrames,
              FifoSource::kTrimToFrames);
    m_last_audio_us = now_us;
}

// ---------------------------------------------------------------- server

bool ApiServer::open(const std::string& path, uid_t uid, gid_t gid, std::string& error)
{
    m_path = path;
    ::unlink(path.c_str());  // a stale socket from a previous run
    m_listen.reset(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (!m_listen || path.size() >= sizeof(addr.sun_path)) {
        error = "cannot create API socket";
        return false;
    }
    std::strcpy(addr.sun_path, path.c_str());
    if (::bind(m_listen.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(m_listen.get(), 8) != 0) {
        error = path + ": " + std::strerror(errno);
        return false;
    }
    // Only the desktop user (and root) may connect.
    if (::chown(path.c_str(), uid, gid) != 0 || ::chmod(path.c_str(), 0660) != 0) {
        error = path + ": cannot set owner: " + std::strerror(errno);
        return false;
    }
    return true;
}

ApiServer::~ApiServer()
{
    if (m_listen)
        ::unlink(m_path.c_str());
}

std::vector<int> ApiServer::fds() const
{
    std::vector<int> out{m_listen.get()};
    for (const auto& c : m_clients)
        out.push_back(c->fd.get());
    return out;
}

void ApiServer::send_to(ClientConn& c, const Bytes& msg)
{
    // Never block the main loop on a slow app: if its socket buffer is full,
    // that input report is simply skipped (a newer one follows in ~5 ms).
    (void)::send(c.fd.get(), msg.data(), msg.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
}

void ApiServer::drop(size_t i)
{
    log("api: app disconnected");
    m_clients.erase(m_clients.begin() + ptrdiff_t(i));
    if (m_clients.empty())
        m_rumble = false;
}

void ApiServer::handle(int fd, int64_t now)
{
    if (fd == m_listen.get()) {
        const int c = ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (c >= 0) {
            auto conn = std::make_unique<ClientConn>();
            conn->fd.reset(c);
            m_clients.push_back(std::move(conn));
            log("api: app connected ({} total)", m_clients.size());
        }
        return;
    }
    for (size_t i = 0; i < m_clients.size(); ++i) {
        ClientConn& c = *m_clients[i];
        if (c.fd.get() != fd)
            continue;
        uint8_t chunk[65536];
        for (;;) {
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
                drop(i);
                return;
            }
            if (n < 0)
                break;
            c.buffer.insert(c.buffer.end(), chunk, chunk + n);
        }
        bool bad = false;
        while (auto m = wire::take_message(c.buffer, bad)) {
            switch (m->type) {
            case wire::Type::Hello: {
                const uint32_t version = m->body.size() >= 4 ? read_le32(m->body.data()) : 0;
                if (version != wire::kVersion) {
                    log("api: app uses protocol version {}, padlinkd speaks {}", version, wire::kVersion);
                    drop(i);
                    return;
                }
                c.hello = true;
                Bytes v{uint8_t(wire::kVersion), 0, 0, 0};
                send_to(c, wire::message(wire::Type::Hello, v));
                const uint8_t pad = m_pad_connected;
                send_to(c, wire::message(wire::Type::PadState, ByteView(&pad, 1)));
                break;
            }
            case wire::Type::VideoI420:
                if (c.hello && video && m->body.size() == wire::kI420Size)
                    video->submit(m->body, now);
                break;
            case wire::Type::AudioPcm:
                if (c.hello && audio)
                    audio->submit(m->body, now);
                break;
            case wire::Type::Rumble:
                if (c.hello && !m->body.empty())
                    m_rumble = m->body[0] != 0;
                break;
            default:
                break;  // unknown messages are ignored (forward compatibility)
            }
        }
        if (bad) {
            log("api: malformed message from app");
            drop(i);
        }
        return;
    }
}

void ApiServer::broadcast_input(const GamepadState& s)
{
    if (m_clients.empty())
        return;
    const Bytes msg = wire::message(wire::Type::Input, wire::encode_state(s));
    for (auto& c : m_clients)
        if (c->hello)
            send_to(*c, msg);
}

void ApiServer::broadcast_pad_connected(bool connected)
{
    m_pad_connected = connected;
    const uint8_t b = connected;
    const Bytes msg = wire::message(wire::Type::PadState, ByteView(&b, 1));
    for (auto& c : m_clients)
        if (c->hello)
            send_to(*c, msg);
}

} // namespace padlink
