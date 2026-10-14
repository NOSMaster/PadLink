// SPDX-License-Identifier: GPL-2.0-or-later
// padlinkd's side of the app API: a Unix socket that apps (via libpadlink)
// connect to. Apps can send video/audio/rumble and receive GamePad input.
#pragma once

#include "audio/audio_source.h"
#include "padlink/padlink.h"
#include "protocol/bytes.h"
#include "util/unique_fd.h"
#include "video/frame_source.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace padlink {

// Video frames sent by apps, shared between the API server (writer) and the
// video thread (reader). Falls back to another source when no app is sending.
class ApiVideoSource : public FrameSource {
public:
    explicit ApiVideoSource(std::unique_ptr<FrameSource> fallback) : m_fallback(std::move(fallback)) {}
    bool latest(uint8_t* out) override;
    void submit(ByteView i420, int64_t now_us);
    bool app_active(int64_t now_us) const { return now_us - m_last_frame_us < 1000000; }

private:
    std::unique_ptr<FrameSource> m_fallback;
    std::mutex m_mutex;
    Bytes m_frame;
    std::atomic<int64_t> m_last_frame_us{-1000000000};
};

// Same idea for audio.
class ApiAudioSource : public AudioSource {
public:
    explicit ApiAudioSource(std::unique_ptr<AudioSource> fallback);
    size_t read(int16_t* out, size_t frames) override;
    void submit(ByteView pcm, int64_t now_us);

private:
    std::unique_ptr<AudioSource> m_fallback;
    std::mutex m_mutex;
    std::vector<int16_t> m_ring;
    size_t m_head = 0, m_count = 0;
    std::atomic<int64_t> m_last_audio_us{-1000000000};
};

class ApiServer {
public:
    // Listens on `path`; the socket file is given to `owner_uid`/`owner_gid`
    // (the user who ran sudo) with mode 0660, so only that user's apps connect.
    bool open(const std::string& path, uid_t owner_uid, gid_t owner_gid, std::string& error);
    ~ApiServer();

    // poll() descriptors: the listening socket plus every client.
    std::vector<int> fds() const;
    // Handles whatever is readable on `fd` (accept / client messages).
    void handle(int fd, int64_t now_us);

    void broadcast_input(const GamepadState& s);
    void broadcast_pad_connected(bool connected);

    // Where app media goes (set before clients connect; may be null).
    ApiVideoSource* video = nullptr;
    ApiAudioSource* audio = nullptr;
    bool rumble() const { return m_rumble; }
    size_t client_count() const { return m_clients.size(); }

private:
    struct ClientConn {
        UniqueFd fd;
        Bytes buffer;
        bool hello = false;
    };
    void send_to(ClientConn& c, const Bytes& msg);
    void drop(size_t index);

    std::string m_path;
    UniqueFd m_listen;
    std::vector<std::unique_ptr<ClientConn>> m_clients;
    bool m_pad_connected = false;
    std::atomic<bool> m_rumble{false};
};

} // namespace padlink
