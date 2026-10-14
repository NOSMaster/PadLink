// SPDX-License-Identifier: GPL-2.0-or-later
#include "doctest.h"
#include "audio/audio_source.h"
#include "protocol/astrm.h"

using namespace padlink;

TEST_CASE("ASTRM PCM header matches the documented real-console example")
{
    // seq 0x321, ts 0x12345678, no rumble -> 23 21 06 80 78 56 34 12
    const Bytes pcm(kAudioPayloadBytes, 0);
    const Bytes p = build_audio_packet({0x321, false, 0x12345678}, pcm);
    REQUIRE(p.size() == 8 + 1664);
    CHECK(Bytes(p.begin(), p.begin() + 8) == Bytes{0x23, 0x21, 0x06, 0x80, 0x78, 0x56, 0x34, 0x12});
}

TEST_CASE("rumble sets the vibrate bit; packets parse back")
{
    Bytes pcm(kAudioPayloadBytes);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = uint8_t(i);
    const Bytes p = build_audio_packet({5, true, 99}, pcm);
    CHECK(p[0] == (0x20 | 0x08));
    const auto a = parse_astrm_packet(p);
    REQUIRE(a);
    CHECK(a->format == 1);
    CHECK(a->header.vibrate);
    CHECK_FALSE(a->is_video_format);
    CHECK(a->header.sequence == 5);
    CHECK(a->header.timestamp == 99);
    CHECK(a->payload == pcm);
}

TEST_CASE("the video-format message is recognized as such")
{
    const Bytes f{0x04, 0, 0, 0, 0x10, 0x10, 0x81, 0x08, 1, 2, 3, 4};
    const auto a = parse_astrm_packet(f);
    REQUIRE(a);
    CHECK(a->is_video_format);
}

TEST_CASE("ring buffer: FIFO order, silence on underrun")
{
    std::vector<int16_t> ring(2 * 16);
    size_t head = 0, count = 0;
    const int16_t in[] = {1, -1, 2, -2, 3, -3};
    CHECK(ring_push(ring, head, count, in, 3, 16, 8) == 0);
    int16_t out[8];
    CHECK(ring_pop(ring, head, count, out, 4) == 3);
    CHECK(out[0] == 1);
    CHECK(out[5] == -3);
    CHECK(out[6] == 0);  // 4th frame: silence
    CHECK(out[7] == 0);
    CHECK(count == 0);
}

TEST_CASE("ring buffer: latency is trimmed by dropping the oldest audio")
{
    std::vector<int16_t> ring(2 * 100);
    size_t head = 0, count = 0;
    std::vector<int16_t> in(2 * 30);
    for (size_t i = 0; i < 30; ++i)
        in[2 * i] = in[2 * i + 1] = int16_t(i);
    // max 20 buffered frames, trim to 10 -> keeps frames 20..29
    CHECK(ring_push(ring, head, count, in.data(), 30, 20, 10) == 20);
    CHECK(count == 10);
    int16_t out[2];
    ring_pop(ring, head, count, out, 1);
    CHECK(out[0] == 20);
}

TEST_CASE("test tone beeps for 200 ms each second")
{
    ToneSource tone;
    std::vector<int16_t> buf(2 * kAudioRate);
    CHECK(tone.read(buf.data(), kAudioRate) == size_t(kAudioRate));
    int peak_on = 0, peak_off = 0;
    for (int i = 0; i < kAudioRate; ++i) {
        const int v = std::abs(int(buf[2 * i]));
        (i < kAudioRate / 5 ? peak_on : peak_off) = std::max(i < kAudioRate / 5 ? peak_on : peak_off, v);
    }
    CHECK(peak_on > 5000);
    CHECK(peak_off == 0);
}

#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <thread>
#include <unistd.h>

TEST_CASE("FifoSource receives PCM written into its FIFO")
{
    const auto path = std::filesystem::temp_directory_path() / ("padlink-fifo-test-" + std::to_string(::getpid()));
    {
        FifoSource src(path.string());
        // Wait for the reader thread to open the FIFO, then write 3 frames.
        int fd = -1;
        for (int i = 0; i < 100 && fd < 0; ++i) {
            fd = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
            if (fd < 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(fd >= 0);
        const uint8_t pcm[] = {0x01, 0x00, 0xff, 0xff, 0x02, 0x00, 0xfe, 0xff, 0x03, 0x00, 0xfd, 0xff};
        REQUIRE(::write(fd, pcm, sizeof(pcm)) == ssize_t(sizeof(pcm)));
        ::close(fd);
        int16_t out[8] = {};
        size_t got = 0;
        for (int i = 0; i < 100 && got == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            got = src.read(out, 4);
        }
        CHECK(got == 3);
        CHECK(out[0] == 1);
        CHECK(out[1] == -1);
        CHECK(out[4] == 3);
        CHECK(out[5] == -3);
        CHECK(out[6] == 0);
    }
    std::filesystem::remove(path);
}
