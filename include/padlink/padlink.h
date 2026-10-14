// SPDX-License-Identifier: GPL-2.0-or-later
// padlink public API: lets an application (an emulator, a recompiled game,
// a demo...) draw on a Wii U GamePad, play sound on it, make it rumble and
// read its input. The app talks to padlinkd over a local socket, so it does
// not need root.
//
//   auto pad = padlink::Client::connect();
//   pad->send_frame_rgba(pixels, width, height, width * 4);
//   if (auto s = pad->state(); s && (s->buttons & padlink::button::A)) ...
//
// Link with the `padlink` library (CMake: target_link_libraries(app padlink)).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace padlink {

// Screen size of the GamePad in pixels.
inline constexpr int kScreenWidth = 854;
inline constexpr int kScreenHeight = 480;

// Audio format the GamePad plays: 48 kHz, stereo, signed 16-bit.
inline constexpr int kAudioRate = 48000;

namespace button {
inline constexpr uint32_t Sync = 0x000001, Home = 0x000002, Minus = 0x000004, Plus = 0x000008,
                          R = 0x000010, L = 0x000020, ZR = 0x000040, ZL = 0x000080,
                          Down = 0x000100, Up = 0x000200, Right = 0x000400, Left = 0x000800,
                          Y = 0x001000, X = 0x002000, B = 0x004000, A = 0x008000,
                          TV = 0x200000, R3 = 0x400000, L3 = 0x800000;
}

struct GamepadState {
    uint16_t sequence = 0;      // increments with every report (~180 per second)
    uint32_t buttons = 0;       // padlink::button bits that are held down
    // Sticks from -1 to 1. X: right is positive. Y: UP is positive.
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0;
    bool touch_pressed = false;
    int touch_x = 0, touch_y = 0;  // screen pixels (0..853, 0..479), valid while pressed
    uint16_t touch_pressure = 0;
    std::array<int16_t, 3> accel{};  // raw accelerometer
    std::array<int32_t, 3> gyro{};   // raw gyroscope
    uint8_t battery = 0;
    uint8_t volume = 0;              // the GamePad's volume slider
    uint8_t power_status = 0;        // 0x01 charger plugged in, 0x40 charging
};

class Client {
public:
    // Connects to padlinkd. Returns nullptr (and fills `error`) if padlinkd is
    // not running or the socket is not accessible.
    static std::unique_ptr<Client> connect(const std::string& socket_path = "/run/padlink/padlink.sock",
                                           std::string* error = nullptr);
    ~Client();

    // True while a GamePad is connected to padlinkd.
    bool gamepad_connected() const;
    // The most recent input, or std::nullopt if none has arrived yet.
    std::optional<GamepadState> state() const;
    // Called on a background thread for every input report. Keep it short.
    void on_input(std::function<void(const GamepadState&)> callback);

    // Video. Frames replace whatever padlinkd showed before (test pattern or
    // screen capture) while your app keeps sending them. Send at up to ~60 fps.
    // RGBA: 4 bytes per pixel (R, G, B, A), any size: it is scaled to fit
    // the screen. `stride` is the number of bytes per row.
    bool send_frame_rgba(const uint8_t* rgba, int width, int height, int stride);
    // Already in the GamePad's native format: 864x480 yuv420p (the right 10
    // columns are not visible), 622080 bytes.
    bool send_frame_i420(const uint8_t* i420);

    // Audio: interleaved stereo samples (left, right, left, right...).
    // Send it in real time; padlinkd buffers about 40-100 ms.
    bool send_audio(const int16_t* stereo_samples, size_t frames);

    // Rumble motor on or off.
    bool set_rumble(bool on);

    // false once the connection to padlinkd is lost.
    bool connected() const;

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

private:
    Client();
    struct Impl;  // implementation details live in padlink_client.cpp
    std::unique_ptr<Impl> m;
};

// Converts and scales an RGBA image into an 864x480 yuv420p frame (keeping
// the aspect ratio, with black borders if needed). `out` must hold 622080
// bytes. Used by send_frame_rgba(); exposed for apps that want to cache it.
void rgba_to_gamepad_i420(const uint8_t* rgba, int width, int height, int stride, uint8_t* out);

} // namespace padlink
