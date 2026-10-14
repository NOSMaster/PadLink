// SPDX-License-Identifier: GPL-2.0-or-later
// libpadlink: the app side of the padlinkd socket protocol (api/wire.h).
#include "padlink/padlink.h"

#include "api/wire.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace padlink {

struct Client::Impl {
    int fd = -1;
    std::mutex write_mutex;  // app threads may send concurrently
    mutable std::mutex state_mutex;
    std::optional<GamepadState> state;
    std::function<void(const GamepadState&)> callback;
    std::atomic<bool> pad_connected{false}, alive{true}, stop{false};
    std::thread reader;
    Bytes frame;  // scratch buffer for RGBA -> I420

    bool send(wire::Type type, ByteView body)
    {
        const Bytes msg = wire::message(type, body);
        std::lock_guard lock(write_mutex);
        size_t off = 0;
        while (off < msg.size() && alive) {
            const ssize_t n = ::send(fd, msg.data() + off, msg.size() - off, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                alive = false;
                return false;
            }
            off += size_t(n);
        }
        return alive;
    }

    void read_loop()
    {
        Bytes buffer;
        uint8_t chunk[4096];
        while (!stop) {
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, 200) <= 0)
                continue;
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) {
                alive = false;
                pad_connected = false;
                return;
            }
            buffer.insert(buffer.end(), chunk, chunk + n);
            bool bad = false;
            while (auto m = wire::take_message(buffer, bad)) {
                if (m->type == wire::Type::PadState && !m->body.empty()) {
                    pad_connected = m->body[0] != 0;
                } else if (m->type == wire::Type::Input) {
                    if (auto s = wire::decode_state(m->body)) {
                        std::function<void(const GamepadState&)> cb;
                        {
                            std::lock_guard lock(state_mutex);
                            state = *s;
                            cb = callback;
                        }
                        if (cb)
                            cb(*s);
                    }
                }
            }
            if (bad) {
                alive = false;
                return;
            }
        }
    }
};

Client::Client() : m(std::make_unique<Impl>()) {}

Client::~Client()
{
    m->stop = true;
    if (m->reader.joinable())
        m->reader.join();
    if (m->fd >= 0)
        ::close(m->fd);
}

std::unique_ptr<Client> Client::connect(const std::string& path, std::string* error)
{
    auto fail = [&](const std::string& why) -> std::unique_ptr<Client> {
        if (error)
            *error = why;
        return nullptr;
    };
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return fail(std::string("socket: ") + std::strerror(errno));
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return fail("socket path too long");
    }
    std::strcpy(addr.sun_path, path.c_str());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const std::string why = path + ": " + std::strerror(errno) + " (is padlinkd running?)";
        ::close(fd);
        return fail(why);
    }
    std::unique_ptr<Client> c(new Client());
    c->m->fd = fd;
    Bytes version;
    for (int i = 0; i < 4; ++i)
        version.push_back(uint8_t(wire::kVersion >> (8 * i)));
    if (!c->m->send(wire::Type::Hello, version))
        return fail("padlinkd closed the connection");
    c->m->reader = std::thread([impl = c->m.get()] { impl->read_loop(); });
    return c;
}

bool Client::gamepad_connected() const { return m->pad_connected; }
bool Client::connected() const { return m->alive; }

std::optional<GamepadState> Client::state() const
{
    std::lock_guard lock(m->state_mutex);
    return m->state;
}

void Client::on_input(std::function<void(const GamepadState&)> callback)
{
    std::lock_guard lock(m->state_mutex);
    m->callback = std::move(callback);
}

bool Client::send_frame_i420(const uint8_t* i420)
{
    return m->send(wire::Type::VideoI420, ByteView(i420, wire::kI420Size));
}

bool Client::send_frame_rgba(const uint8_t* rgba, int width, int height, int stride)
{
    m->frame.resize(wire::kI420Size);
    rgba_to_gamepad_i420(rgba, width, height, stride, m->frame.data());
    return send_frame_i420(m->frame.data());
}

bool Client::send_audio(const int16_t* samples, size_t frames)
{
    // Samples are sent as little-endian s16, which is the native layout on x86/ARM.
    return m->send(wire::Type::AudioPcm, ByteView(reinterpret_cast<const uint8_t*>(samples), frames * 4));
}

bool Client::set_rumble(bool on)
{
    const uint8_t b = on;
    return m->send(wire::Type::Rumble, ByteView(&b, 1));
}

} // namespace padlink
