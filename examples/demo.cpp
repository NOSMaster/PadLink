// SPDX-License-Identifier: GPL-2.0-or-later
// padlink-demo: a tiny app that uses only the public padlink API.
//   left stick  moves the square        touch  draws dots
//   A (hold)    rumble                  B (hold) plays a tone
//   buttons     light up the row of boxes at the bottom
// Run padlinkd first (sudo build/padlinkd run), then: build/padlink-demo

#include <padlink/padlink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_quit{false};

struct Canvas {
    int w = padlink::kScreenWidth, h = padlink::kScreenHeight;
    std::vector<uint8_t> px = std::vector<uint8_t>(size_t(w) * h * 4);

    void rect(int x0, int y0, int rw, int rh, uint8_t r, uint8_t g, uint8_t b)
    {
        for (int y = std::max(0, y0); y < std::min(h, y0 + rh); ++y)
            for (int x = std::max(0, x0); x < std::min(w, x0 + rw); ++x) {
                uint8_t* p = &px[(size_t(y) * w + x) * 4];
                p[0] = r, p[1] = g, p[2] = b, p[3] = 255;
            }
    }
};

} // namespace

int main()
{
    std::signal(SIGINT, [](int) { g_quit = true; });

    std::string error;
    auto pad = padlink::Client::connect("/run/padlink/padlink.sock", &error);
    if (!pad) {
        std::fprintf(stderr, "cannot connect: %s\n", error.c_str());
        return 1;
    }
    std::printf("connected to padlinkd. Ctrl+C to quit.\n");

    Canvas canvas;
    std::vector<std::pair<int, int>> dots;
    float box_x = 400, box_y = 220;
    double tone_phase = 0;
    bool rumbling = false, was_pad = false;
    std::vector<int16_t> audio(2 * padlink::kAudioRate / 60);  // 800 frames = 1/60 s

    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    for (uint64_t frame = 0; !g_quit && pad->connected(); ++frame) {
        if (pad->gamepad_connected() != was_pad) {
            was_pad = pad->gamepad_connected();
            std::printf("GamePad %s\n", was_pad ? "connected" : "disconnected");
        }
        const padlink::GamepadState s = pad->state().value_or(padlink::GamepadState{});

        // Update: the stick moves the box (Y up is positive in the API).
        box_x = std::clamp(box_x + s.left_x * 6, 0.0f, float(canvas.w - 60));
        box_y = std::clamp(box_y - s.left_y * 6, 0.0f, float(canvas.h - 120));
        if (s.touch_pressed && dots.size() < 2000)
            dots.emplace_back(s.touch_x, s.touch_y);
        if (s.buttons & padlink::button::X)
            dots.clear();  // X clears the drawing
        const bool want_rumble = s.buttons & padlink::button::A;
        if (want_rumble != rumbling)
            pad->set_rumble(rumbling = want_rumble);

        // Draw: a slowly shifting background, the box, dots, button lights.
        const uint8_t shade = uint8_t(40 + 30 * std::sin(frame / 60.0));
        canvas.rect(0, 0, canvas.w, canvas.h, 10, 20, shade);
        canvas.rect(int(box_x), int(box_y), 60, 60, 240, 200, 40);
        for (auto [x, y] : dots)
            canvas.rect(x - 3, y - 3, 6, 6, 255, 255, 255);
        const uint32_t order[] = {padlink::button::A, padlink::button::B, padlink::button::X, padlink::button::Y,
                                  padlink::button::L, padlink::button::R, padlink::button::ZL, padlink::button::ZR,
                                  padlink::button::Up, padlink::button::Down, padlink::button::Left,
                                  padlink::button::Right, padlink::button::Plus, padlink::button::Minus,
                                  padlink::button::Home, padlink::button::L3, padlink::button::R3};
        for (size_t i = 0; i < std::size(order); ++i) {
            const bool on = s.buttons & order[i];
            canvas.rect(20 + int(i) * 48, 430, 40, 30, on ? 60 : 50, on ? 220 : 50, on ? 90 : 60);
        }
        pad->send_frame_rgba(canvas.px.data(), canvas.w, canvas.h, canvas.w * 4);

        // Audio for this 1/60 s: a 440 Hz tone while B is held, else silence.
        const bool tone = s.buttons & padlink::button::B;
        for (size_t i = 0; i < audio.size() / 2; ++i) {
            const int16_t v = tone ? int16_t(8000 * std::sin(tone_phase)) : 0;
            tone_phase += 2 * M_PI * 440 / padlink::kAudioRate;
            audio[2 * i] = audio[2 * i + 1] = v;
        }
        pad->send_audio(audio.data(), audio.size() / 2);

        next += std::chrono::microseconds(1000000 / 60);
        std::this_thread::sleep_until(next);
    }
    pad->set_rumble(false);
    std::printf("bye\n");
    return 0;
}
