// SPDX-License-Identifier: GPL-2.0-or-later
#include "doctest.h"
#include "api/api_server.h"
#include "api/wire.h"
#include "clock/tsf_clock.h"

#include <chrono>
#include <filesystem>
#include <thread>
#include <unistd.h>

using namespace padlink;

TEST_CASE("wire messages: framing, partial data, malformed length")
{
    Bytes stream = wire::message(wire::Type::Rumble, Bytes{1});
    put_bytes(stream, wire::message(wire::Type::AudioPcm, Bytes(8, 7)));
    Bytes partial(stream.begin(), stream.begin() + 7);  // first message + 1 byte
    bool bad = false;
    auto m = wire::take_message(partial, bad);
    REQUIRE(m);
    CHECK(m->type == wire::Type::Rumble);
    CHECK(m->body == Bytes{1});
    CHECK_FALSE(wire::take_message(partial, bad));  // second one incomplete
    CHECK_FALSE(bad);

    Bytes huge{0xff, 0xff, 0xff, 0x7f, 2};
    CHECK_FALSE(wire::take_message(huge, bad));
    CHECK(bad);
}

TEST_CASE("GamepadState round-trips through the wire format")
{
    GamepadState s;
    s.sequence = 1234;
    s.buttons = button::A | button::L3;
    s.left_x = -0.5f;
    s.right_y = 0.25f;
    s.touch_pressed = true;
    s.touch_x = 853;
    s.touch_y = 479;
    s.touch_pressure = 4095;
    s.accel = {-100, 200, -300};
    s.gyro = {-8388608, 0, 8388607};
    s.battery = 166;
    s.volume = 33;
    s.power_status = 0x41;
    const auto back = wire::decode_state(wire::encode_state(s));
    REQUIRE(back);
    CHECK(back->sequence == 1234);
    CHECK(back->buttons == s.buttons);
    CHECK(back->left_x == -0.5f);
    CHECK(back->right_y == 0.25f);
    CHECK(back->touch_pressed);
    CHECK(back->touch_x == 853);
    CHECK(back->touch_y == 479);
    CHECK(back->accel == s.accel);
    CHECK(back->gyro == s.gyro);
    CHECK(back->battery == 166);
    CHECK(back->power_status == 0x41);
    CHECK_FALSE(wire::decode_state(Bytes(10, 0)));
}

TEST_CASE("RGBA conversion: colours, letterboxing, black borders")
{
    // A 2:1 image (wider than the 854x480 screen) gets black bars top and bottom.
    const int w = 200, h = 100;
    std::vector<uint8_t> red(size_t(w) * h * 4);
    for (size_t i = 0; i < red.size(); i += 4)
        red[i] = 255, red[i + 3] = 255;
    std::vector<uint8_t> out(864 * 480 * 3 / 2);
    rgba_to_gamepad_i420(red.data(), w, h, w * 4, out.data());
    const uint8_t* Y = out.data();
    const uint8_t* V = Y + 864 * 480 + 864 * 480 / 4;
    CHECK(Y[0] == 16);                    // top bar: black
    CHECK(Y[240 * 864 + 427] == 82);      // middle: red's luma (BT.601)
    CHECK(V[120 * 432 + 213] == 240);     // red's Cr
    CHECK(Y[240 * 864 + 860] == 16);      // hidden columns stay black
}

TEST_CASE("client and server talk over a real socket")
{
    const auto path = (std::filesystem::temp_directory_path() / ("padlink-api-test-" + std::to_string(::getpid()))).string();
    ApiServer server;
    std::string error;
    REQUIRE_MESSAGE(server.open(path, ::getuid(), ::getgid(), error), error);
    ApiVideoSource video(nullptr);
    ApiAudioSource audio(nullptr);
    server.video = &video;
    server.audio = &audio;

    // Pump the server for a while (it normally runs in padlinkd's poll loop).
    auto pump = [&](int ms) {
        for (int i = 0; i < ms; ++i) {
            for (int fd : server.fds())
                server.handle(fd, monotonic_us());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    auto client = Client::connect(path, &error);
    REQUIRE_MESSAGE(client, error);
    pump(50);
    CHECK(server.client_count() == 1);

    server.broadcast_pad_connected(true);
    GamepadState s;
    s.buttons = button::B;
    s.left_y = 1.0f;
    server.broadcast_input(s);
    for (int i = 0; i < 100 && !client->state(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(client->state());
    CHECK(client->state()->buttons == button::B);
    CHECK(client->gamepad_connected());

    // A frame (622 KB) is bigger than a socket buffer, so the app side must
    // send from another thread while the server side reads (as in padlinkd).
    bool sent = false;
    std::thread app([&] {
        std::vector<uint8_t> frame(wire::kI420Size, 77);
        const int16_t pcm[4] = {100, -100, 200, -200};
        sent = client->set_rumble(true) && client->send_frame_i420(frame.data()) && client->send_audio(pcm, 2);
    });
    pump(200);
    app.join();
    CHECK(sent);
    CHECK(server.rumble());
    std::vector<uint8_t> got(wire::kI420Size);
    REQUIRE(video.latest(got.data()));
    CHECK(got[1000] == 77);
    int16_t out[4];
    CHECK(audio.read(out, 2) == 2);
    CHECK(out[2] == 200);

    client.reset();  // app quits
    pump(50);
    CHECK(server.client_count() == 0);
    CHECK_FALSE(server.rumble());  // rumble stops with the last app
}
