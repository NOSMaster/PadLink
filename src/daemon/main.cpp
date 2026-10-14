// SPDX-License-Identifier: GPL-2.0-or-later
// padlinkd — pair a Wii U GamePad, host its access point, expose its input.
//
//   sudo padlinkd pair         pair a GamePad (WPS), then continue with `run`
//   sudo padlinkd run          host the paired GamePad's AP + DHCP
//   sudo padlinkd clock-probe  check the AP TSF media clock
//   sudo padlinkd status       show the stored pairing (without the key)
//
// Run scripts/padlink-net-up.sh first so NetworkManager releases the card.

#include "clock/tsf_clock.h"
#include "protocol/bytes.h"
#include "protocol/pairing.h"
#include "protocol/wps_credential.h"
#include "store/pairing_store.h"
#include "util/log.h"
#include "util/process.h"
#include "util/unique_fd.h"
#include "wifi/dhcp_server.h"
#include "wifi/hostapd.h"
#include "wifi/hostapd_config.h"
#include "input/virtual_gamepad.h"
#include "net/drc_sockets.h"
#include "protocol/cmd.h"
#include "protocol/input.h"
#include "video/video_sender.h"
#include "audio/audio_sender.h"
#include "api/api_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace padlink;
namespace fs = std::filesystem;

namespace {

constexpr const char* kRunDir = "/run/padlink";
constexpr const char* kNetState = "/run/padlink/net-state";
constexpr const char* kDefaultStore = "/var/lib/padlink/pairing.conf";
constexpr const char* kConsoleIp = "192.168.1.10";
constexpr const char* kApiSocket = "/run/padlink/padlink.sock";

struct Options {
    std::string command;
    std::string iface = "wlan0";
    int channel = 36;
    std::string hostapd;
    std::string store = kDefaultStore;
    int seconds = 10;
    bool no_run = false;
    bool no_uinput = false;
    bool verbose_input = false;
    std::string video = "pattern";
    std::string chunk_split = "rows";
    int idr_every = 0;  // only when the GamePad asks (clean on hardware)
    std::string video_mode = "normal";
    bool answer_idr = true, init_on_idr = true;
    std::string audio = "none";
    std::string audio_fifo = "/tmp/padlink-audio";
    bool rumble_test = false;
};

void usage()
{
    std::fprintf(stderr,
        "usage: padlinkd <pair|run|clock-probe|status> [options]\n"
        "  --iface NAME      Wi-Fi interface (default wlan0)\n"
        "  --channel N       5 GHz channel for pairing (default 36)\n"
        "  --hostapd PATH    patched hostapd (default: next to padlinkd)\n"
        "  --store PATH      pairing file (default %s)\n"
        "  --seconds N       clock-probe duration (default 10)\n"
        "  --no-run          pair: stop after pairing\n"
        "  --no-uinput       run: do not create virtual input devices\n"
        "  --verbose-input   run: log button/stick/touch changes\n"
        "  --video SOURCE    run: pattern (default) | stdin (raw 864x480 yuv420p) | none\n"
        "  --chunk-split M   run: rows (default) | late | even  (video chunk boundaries)\n"
        "  --idr-every N     run: also send a keyframe every N frames (default 0 = only on request)\n"
        "  --video-mode M    run: normal (default) | fullpel  (encoder experiment)\n"
        "  --ignore-idr-requests  run: never answer the GamePad's keyframe requests\n"
        "  --no-init-on-idr  run: init flag only on the first keyframe\n"
        "  --audio SOURCE    run: none (default) | tone | fifo\n"
        "  --audio-fifo PATH run: FIFO for --audio fifo (default /tmp/padlink-audio)\n"
        "  --rumble-test     run: pulse the rumble motor (0.5 s every 3 s)\n",
        kDefaultStore);
}

std::optional<Options> parse_args(int argc, char** argv)
{
    if (argc < 2)
        return std::nullopt;
    Options o;
    o.command = argv[1];
    o.hostapd = (fs::canonical("/proc/self/exe").parent_path() / "padlink-hostapd").string();
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        try {
            if (a == "--iface") o.iface = value();
            else if (a == "--channel") o.channel = std::stoi(value());
            else if (a == "--hostapd") o.hostapd = value();
            else if (a == "--store") o.store = value();
            else if (a == "--seconds") o.seconds = std::stoi(value());
            else if (a == "--no-run") o.no_run = true;
            else if (a == "--no-uinput") o.no_uinput = true;
            else if (a == "--verbose-input") o.verbose_input = true;
            else if (a == "--video") o.video = value();
            else if (a == "--chunk-split") o.chunk_split = value();
            else if (a == "--idr-every") o.idr_every = std::stoi(value());
            else if (a == "--video-mode") o.video_mode = value();
            else if (a == "--ignore-idr-requests") o.answer_idr = false;
            else if (a == "--audio") o.audio = value();
            else if (a == "--audio-fifo") o.audio_fifo = value();
            else if (a == "--rumble-test") o.rumble_test = true;
            else if (a == "--no-init-on-idr") o.init_on_idr = false;
            else return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }
    return o;
}

// ---------------------------------------------------------------- helpers

std::optional<MacAddress> interface_mac(const std::string& iface)
{
    std::ifstream in("/sys/class/net/" + iface + "/address");
    std::string text;
    if (!std::getline(in, text))
        return std::nullopt;
    return MacAddress::parse(text);
}

std::string random_psk_hex()
{
    uint8_t key[32];
    if (getrandom(key, sizeof(key), 0) != sizeof(key))
        throw std::runtime_error("getrandom failed");
    return to_hex(key);
}

bool write_private_file(const std::string& path, ByteView data, std::string& error)
{
    UniqueFd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
    if (!fd || ::write(fd.get(), data.data(), data.size()) != ssize_t(data.size())) {
        error = path + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

bool write_private_text(const std::string& path, const std::string& text, std::string& error)
{
    return write_private_file(path, ByteView(reinterpret_cast<const uint8_t*>(text.data()), text.size()), error);
}

bool configure_ip(const std::string& iface)
{
    const bool ok = run_command({"ip", "addr", "flush", "dev", iface}) == 0 &&
                    run_command({"ip", "addr", "add", std::string(kConsoleIp) + "/24", "brd", "+", "dev", iface}) == 0 &&
                    run_command({"ip", "link", "set", "dev", iface, "mtu", "1800"}) == 0;
    if (ok)
        log("network: {} is {}/24, MTU 1800", iface, kConsoleIp);
    else
        log("network: failed to configure {} (is the `ip` command available?)", iface);
    return ok;
}

// SIGINT/SIGTERM become readable events on a descriptor, so Ctrl+C is handled
// in the normal poll() loop and every destructor (hostapd shutdown!) runs.
UniqueFd make_signal_fd()
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    sigprocmask(SIG_BLOCK, &set, nullptr);
    return UniqueFd(signalfd(-1, &set, SFD_CLOEXEC));
}

bool preflight(const Options& o)
{
    if (geteuid() != 0) {
        log("padlinkd must run as root: sudo {} {}", "padlinkd", o.command);
        return false;
    }
    if (!fs::exists("/sys/class/net/" + o.iface)) {
        log("interface {} does not exist", o.iface);
        return false;
    }
    if (!fs::exists(kNetState))
        log("warning: {} not found. Did you run scripts/padlink-net-up.sh {}?", kNetState, o.iface);
    if (!fs::exists(o.hostapd)) {
        log("patched hostapd not found at {} (build it with cmake --build build)", o.hostapd);
        return false;
    }
    fs::create_directories(kRunDir);
    // 0711: others may reach files they know the name of (the API socket,
    // which has its own permissions) but cannot list the directory.
    fs::permissions(kRunDir, fs::perms::owner_all | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::replace);
    return true;
}

std::optional<MacAddress> mac_after(const std::string& event, const std::string& prefix)
{
    if (event.rfind(prefix, 0) != 0 || event.size() < prefix.size() + 17)
        return std::nullopt;
    return MacAddress::parse(std::string_view(event).substr(prefix.size(), 17));
}

// Converts a raw report into the public API's GamepadState.
GamepadState to_api_state(const InputReport& r, const StickCalibration& c)
{
    GamepadState s;
    s.sequence = r.sequence;
    s.buttons = r.buttons;
    // API convention: up is positive, so Y is not inverted (raw Y grows upward).
    s.left_x = stick_to_axis(r.sticks[0], c.center[0], false) / 32767.0f;
    s.left_y = stick_to_axis(r.sticks[1], c.center[1], false) / 32767.0f;
    s.right_x = stick_to_axis(r.sticks[2], c.center[2], false) / 32767.0f;
    s.right_y = stick_to_axis(r.sticks[3], c.center[3], false) / 32767.0f;
    s.touch_pressed = r.touch.pressed;
    if (r.touch.pressed) {
        const ScreenPoint p = touch_to_screen(r.touch.raw_x, r.touch.raw_y);
        s.touch_x = p.x;
        s.touch_y = p.y;
    }
    s.touch_pressure = r.touch.pressure;
    s.accel = r.accel;
    s.gyro = r.gyro;
    s.battery = r.battery;
    s.volume = r.volume;
    s.power_status = r.power_status;
    return s;
}

std::unique_ptr<TsfClock> open_clock(const std::string& iface)
{
    const auto path = rtw89_read_reg_path(iface);
    if (!path) {
        log("clock: rtw89 debugfs read_reg not found for {}; no AP TSF clock", iface);
        return nullptr;
    }
    log("clock: using AP TSF registers via {}", *path);
    return std::make_unique<TsfClock>([p = *path] { return rtw89_read_ap_tsf(p); }, monotonic_raw_us);
}

// ---------------------------------------------------------------- run

int cmd_run(const Options& o, int signal_fd)
{
    auto rec = load_pairing(o.store);
    if (!rec) {
        log("no pairing stored in {}. Run `padlinkd pair` first.", o.store);
        return 1;
    }

    const std::string conf_path = std::string(kRunDir) + "/hostapd-run.conf";
    std::string error;
    const ApSettings ap{o.iface, std::string(kRunDir) + "/hostapd", rec->ap_mac, rec->channel};
    if (!write_private_text(conf_path, runtime_config(ap, rec->ssid, rec->psk_hex), error)) {
        log("{}", error);
        return 1;
    }

    log("starting GamePad access point on {} channel {}", o.iface, rec->channel);
    auto hostapd = Hostapd::start(o.hostapd, conf_path, ap.ctrl_dir, o.iface, error);
    if (!hostapd || !hostapd->wait_enabled(20000, error)) {
        log("hostapd: {}", error);
        return 1;
    }
    log("access point is up. Turn the GamePad on (or press its power button).");
    if (!configure_ip(o.iface))
        return 1;

    DhcpServer dhcp;
    if (!dhcp.open(o.iface, error)) {
        log("dhcp: {}", error);
        return 1;
    }

    DrcSockets drc;
    if (!drc.open(o.iface, error)) {
        log("sockets: {}", error);
        return 1;
    }

    std::optional<VirtualGamepad> gamepad;
    if (!o.no_uinput) {
        if (auto created = VirtualGamepad::create(error))
            gamepad.emplace(std::move(*created));
        if (!gamepad) {
            log("uinput: {} (is the uinput module loaded? `sudo modprobe uinput`)", error);
            return 1;
        }
        log("uinput: created 'Wii U GamePad (padlink)' + Touchscreen + Motion devices");
    }

    // The console keeps the GamePad's command channel busy: read the UIC
    // config once (it holds touch calibration; kept in memory only) and send
    // the UVC/UAC query about once per second as a keepalive.
    CommandChannel cmd([&](const Bytes& b) { drc.send(DrcPort::Cmd, b); });
    Bytes uic_config;
    bool uvc_seen = false;
    const int64_t t0 = monotonic_us();
    cmd.add(CmdQuery::Generic, build_generic_query(kServicePeripheral, kMethodReadUicConfig), t0, 0,
            [&](const Bytes& reply) {
                const auto g = parse_generic_reply(reply);
                if (g && g->error == 0 && g->data.size() == 772) {
                    uic_config = g->data;
                    log("cmd: UIC config received (UIC firmware {:02x}{:02x}{:02x}{:02x})", g->data[0], g->data[1],
                        g->data[2], g->data[3]);
                } else {
                    log("cmd: unexpected UIC config reply ({} bytes)", reply.size());
                }
            });
    size_t uvc_id = 0;
    Bytes uvc_query = build_uvc_uac_query();
    uvc_id = cmd.add(CmdQuery::UvcUac, uvc_query, t0, 1000000, [&](const Bytes& reply) {
        if (!uvc_seen)
            log("cmd: UVC/UAC keepalive answered ({} bytes)", reply.size());
        uvc_seen = true;
        sync_uvc_uac_query(uvc_query, reply);
        cmd.update_payload(uvc_id, uvc_query);
    });

    auto clock = open_clock(o.iface);

    // App API: the socket belongs to the user who ran sudo.
    ApiServer api;
    {
        const char* uid = std::getenv("SUDO_UID");
        const char* gid = std::getenv("SUDO_GID");
        if (!api.open(kApiSocket, uid ? uid_t(std::atoi(uid)) : 0, gid ? gid_t(std::atoi(gid)) : 0, error))
            log("api: disabled: {}", error);
        else
            log("api: apps can connect to {}", kApiSocket);
    }

    std::unique_ptr<VideoSender> video;
    if (o.video != "none") {
        if (!clock) {
            log("video: disabled (no AP TSF clock; media timestamps would be wrong)");
        } else {
            const VideoMode mode = o.video_mode == "fullpel" ? VideoMode::Fullpel : VideoMode::Normal;
            auto encoder = Encoder::create(error, mode);
            log("video: encoder mode {}, chunk split {}, IDR every {} frames, init on IDR {}, answer requests {}",
                o.video_mode, o.chunk_split, o.idr_every, o.init_on_idr, o.answer_idr);
            if (!encoder) {
                log("video: {}", error);
                return 1;
            }
            encoder->set_chunk_split(o.chunk_split == "even" ? ChunkSplit::Even
                                     : o.chunk_split == "late" ? ChunkSplit::Late
                                                                : ChunkSplit::Rows);
            std::unique_ptr<FrameSource> source;
            if (o.video == "stdin") {
                source = std::make_unique<PipeSource>(STDIN_FILENO);
                log("video: reading 864x480 yuv420p frames from stdin");
            } else {
                source = std::make_unique<TestPattern>();
                log("video: test pattern");
            }
            // Frames from an app take over while it sends them.
            auto api_video = std::make_unique<ApiVideoSource>(std::move(source));
            api.video = api_video.get();
            source = std::move(api_video);
            VideoSender::Options vo;
            vo.periodic_idr_frames = o.idr_every;
            vo.answer_idr_requests = o.answer_idr;
            vo.init_on_every_idr = o.init_on_idr;
            video = std::make_unique<VideoSender>(drc, *clock, std::move(source), std::move(encoder), vo);
        }
    }
    uint64_t last_video_frames = 0;

    std::unique_ptr<AudioSender> audio;
    FifoSource* fifo = nullptr;  // non-owning view, for statistics
    {  // always on: apps may send audio or rumble at any time (silence otherwise)
        if (!clock) {
            log("audio: disabled (no AP TSF clock)");
        } else {
            std::unique_ptr<AudioSource> source;
            if (o.audio == "fifo") {
                auto f = std::make_unique<FifoSource>(o.audio_fifo);
                fifo = f.get();
                source = std::move(f);
                log("audio: reading 48 kHz stereo s16le from FIFO {} (run scripts/padlink-audio.sh as your user)", o.audio_fifo);
            } else if (o.audio == "tone") {
                source = std::make_unique<ToneSource>();
                log("audio: test tone (beep every second)");
            } else {
                source = std::make_unique<SilenceSource>();
            }
            auto api_audio = std::make_unique<ApiAudioSource>(std::move(source));
            api.audio = api_audio.get();
            audio = std::make_unique<AudioSender>(drc, *clock, std::move(api_audio));
        }
    }
    uint64_t last_audio_packets = 0;

    int64_t next_clock_sample = 0, next_report = monotonic_us() + 5000000, last_input = 0;
    uint64_t input_packets = 0, input_total = 0, idr_requests = 0;
    std::optional<InputReport> last;
    bool released = true;      // no GamePad input right now
    bool need_centers = true;  // stick centers not learned since (re)connect
    uint8_t logged_battery = 0;
    StickCalibration sticks;   // also used for the API's normalized sticks
    bool was_connected = false;

    for (;;) {
        std::vector<pollfd> fds = {
            {signal_fd, POLLIN, 0},
            {hostapd->output_fd(), POLLIN, 0},
            {hostapd->event_fd(), POLLIN, 0},
            {dhcp.fd(), POLLIN, 0},
            {drc.fd(DrcPort::Input), POLLIN, 0},
            {drc.fd(DrcPort::Cmd), POLLIN, 0},
            {drc.fd(DrcPort::Msg), POLLIN, 0},
            {drc.fd(DrcPort::Video), POLLIN, 0},
            {drc.fd(DrcPort::Audio), POLLIN, 0},
            {drc.fd(DrcPort::Unknown), POLLIN, 0},
        };
        constexpr size_t kFirstApiFd = 10;
        for (int fd : api.fds())
            if (fd >= 0)
                fds.push_back({fd, POLLIN, 0});
        ::poll(fds.data(), fds.size(), 20);
        const int64_t now = monotonic_us();

        if (fds[0].revents & POLLIN) {
            log("stopping (signal received)");
            return 0;
        }
        if ((fds[1].revents & (POLLIN | POLLHUP)) && !hostapd->drain_output()) {
            log("hostapd exited unexpectedly");
            return 1;
        }
        while (auto ev = hostapd->read_event()) {
            log("event: {}", *ev);
            if (auto mac = mac_after(*ev, "AP-STA-CONNECTED ")) {
                log(">>> GamePad {} associated and passed WPA2 (Nintendo variant)", mac->to_string());
                if (rec->gamepad_mac != mac) {
                    rec->gamepad_mac = mac;
                    if (!save_pairing(o.store, *rec, error))
                        log("store: {}", error);
                }
            } else if (auto mac = mac_after(*ev, "AP-STA-DISCONNECTED ")) {
                log(">>> GamePad {} disconnected", mac->to_string());
            }
        }
        if (fds[3].revents & POLLIN) {
            if (auto mac = dhcp.handle_one())
                log(">>> GamePad {} got IP 192.168.1.11", mac->to_string());
        }

        uint8_t buf[2048];
        ssize_t n;
        if (fds[4].revents & POLLIN) {
            while ((n = drc.receive(DrcPort::Input, buf, sizeof(buf))) > 0) {
                const auto r = parse_input_report(ByteView(buf, size_t(n)));
                if (!r)
                    continue;
                if (input_total++ == 0)
                    log(">>> GamePad input is flowing");
                ++input_packets;
                const int64_t gap = now - last_input;
                last_input = now;
                if (released) {
                    released = false;
                    if (gap > 3000000) {
                        // A real (re)connection, not a short radio gap: start
                        // the console side over cleanly.
                        need_centers = true;
                        uvc_query = build_uvc_uac_query();
                        cmd.update_payload(uvc_id, uvc_query);
                        cmd.restart(now);
                    }
                }
                if (need_centers) {
                    // Learn where the sticks rest, unless one is being held.
                    if (auto c = learn_stick_centers(*r)) {
                        need_centers = false;
                        sticks = *c;
                        if (gamepad)
                            gamepad->set_stick_calibration(*c);
                        log("input: stick centers {} {} {} {}", c->center[0], c->center[1], c->center[2], c->center[3]);
                    }
                }
                if (gamepad)
                    gamepad->update(*r);
                if (api.client_count() > 0)
                    api.broadcast_input(to_api_state(*r, sticks));
                if (o.verbose_input && (!last || last->buttons != r->buttons || last->touch.pressed != r->touch.pressed))
                    log("input: buttons=0x{:06x} sticks={} {} {} {} touch={} raw=({},{}) p={}", r->buttons, r->sticks[0],
                        r->sticks[1], r->sticks[2], r->sticks[3], r->touch.pressed, r->touch.raw_x, r->touch.raw_y,
                        r->touch.pressure);
                if (!last || last->waiting_for_video != r->waiting_for_video)
                    log("gamepad: {}", r->waiting_for_video ? "waiting for video" : "video active");
                // The battery reading flickers by ±1, so only log bigger changes.
                if (!last || std::abs(int(r->battery) - int(logged_battery)) >= 3 ||
                    last->power_status != r->power_status) {
                    log("gamepad: battery {} power status 0x{:02x}", r->battery, r->power_status);
                    logged_battery = r->battery;
                }
                last = r;
            }
        }
        if (fds[5].revents & POLLIN)
            while ((n = drc.receive(DrcPort::Cmd, buf, sizeof(buf))) > 0)
                cmd.on_packet(ByteView(buf, size_t(n)), now);
        if (fds[6].revents & POLLIN) {
            while ((n = drc.receive(DrcPort::Msg, buf, sizeof(buf))) > 0) {
                if (n == 4 && buf[0] == 1 && buf[1] == 0 && buf[2] == 0 && buf[3] == 0) {
                    ++idr_requests;  // "send me a keyframe": the GamePad lost a frame
                    if (video) {
                        // Timing relative to our last IDR tells "it never recovers"
                        // (steady repeats) apart from "each IDR causes a request".
                        if (idr_requests <= 30 || idr_requests % 20 == 0)
                            log("msg: IDR request #{}: {:.1f} ms after our last IDR, {} frames since",
                                idr_requests, (now - video->last_idr_us()) / 1000.0, video->frames_since_idr());
                        video->request_idr();
                    }
                }
                else
                    log("msg: {} bytes: {}", n, to_hex(ByteView(buf, size_t(std::min<ssize_t>(n, 32)))));
            }
        }
        for (size_t i = 7; i < 10; ++i)  // camera/mic/unknown: not used yet, just drain
            if (fds[i].revents & POLLIN)
                while (::recv(fds[i].fd, buf, sizeof(buf), 0) > 0) {}
        for (size_t i = kFirstApiFd; i < fds.size(); ++i)
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR))
                api.handle(fds[i].fd, now);  // may add or remove clients; fds is rebuilt next loop

        // No input for 100 ms (normally ~18 reports): release every button so
        // a game doesn't see a stuck key while the GamePad is out of range.
        if (!released && now - last_input > 100000) {
            if (gamepad)
                gamepad->release_all();
            released = true;
            log("input: lost; all buttons released");
        }
        if (!released)  // only talk to the GamePad while it is there
            cmd.poll(now);
        const bool connected = input_total > 0 && now - last_input < 1000000;
        if (video)  // stream while the GamePad is sending input (allow 1 s gaps)
            video->set_active(connected);
        if (audio) {
            audio->set_active(connected);
            audio->set_rumble(api.rumble() || (o.rumble_test && now % 3000000 < 500000));
        }
        if (connected != was_connected) {
            api.broadcast_pad_connected(connected);
            was_connected = connected;
        }

        if (clock && now >= next_clock_sample) {
            clock->sample();
            next_clock_sample = now + 100000;  // 10 samples per second
        }
        if (now >= next_report) {
            std::string clock_text = "unavailable";
            if (clock) {
                const auto ppm = clock->drift_ppm();
                clock_text = clock->healthy()
                    ? std::format("healthy, drift {:+.2f} ppm vs MONOTONIC_RAW", ppm.value_or(0))
                    : "UNHEALTHY (" + clock->last_error() + ")";
            }
            const auto& s = cmd.stats();
            log("status: input {:.0f}/s, cmd sent {} retries {} replies {} timeouts {}, IDR requests {}, clock {}",
                input_packets / 5.0, s.sent, s.retries, s.replies, s.timeouts, idr_requests, clock_text);
            if (video) {
                const auto& v = video->stats();
                log("video: {:.1f} fps, {} IDR ({} duplicate requests ignored), {} packets, {} kB sent, max encode {} us, "
                    "skipped (clock) {}, errors {}",
                    double(v.frames - last_video_frames) / 5.0, v.idr_frames.load(), v.idr_requests_ignored.load(),
                    v.packets.load(), v.bytes / 1024, v.max_encode_us.load(), v.skipped_clock.load(), v.errors.load());
                last_video_frames = v.frames;
            }
            if (audio) {
                log("audio: {:.0f} packets/s ({} silent so far){}", double(audio->packets() - last_audio_packets) / 5.0,
                    audio->silent_packets(),
                    fifo ? std::format(", FIFO dropped {} / underrun {} frames", fifo->dropped_frames(), fifo->underrun_frames())
                         : std::string());
                last_audio_packets = audio->packets();
            }
            input_packets = 0;
            next_report = now + 5000000;
        }
    }
}

// ---------------------------------------------------------------- pair

int cmd_pair(const Options& o, int signal_fd)
{
    const auto mac = interface_mac(o.iface);
    if (!mac) {
        log("cannot read MAC address of {}", o.iface);
        return 1;
    }

    PairingRecord rec;
    rec.ap_mac = *mac;
    rec.ssid = runtime_ssid(*mac);
    rec.psk_hex = random_psk_hex();
    rec.channel = o.channel;

    std::string error;
    const auto blob = build_wps_credential({rec.ssid, rec.psk_hex, rec.ap_mac});
    const std::string cred_path = std::string(kRunDir) + "/wps-credential.bin";
    const std::string conf_path = std::string(kRunDir) + "/hostapd-pair.conf";
    const ApSettings ap{o.iface, std::string(kRunDir) + "/hostapd", rec.ap_mac, rec.channel};
    if (!blob || !write_private_file(cred_path, *blob, error) ||
        !write_private_text(conf_path, pairing_config(ap, cred_path), error)) {
        log("cannot prepare pairing: {}", error);
        return 1;
    }

    bool paired = false;
    {
        // This block's end destroys `hostapd`, stopping the pairing AP.
        log("starting pairing access point {} on channel {}", pairing_ssid(rec.ap_mac), rec.channel);
        auto hostapd = Hostapd::start(o.hostapd, conf_path, ap.ctrl_dir, o.iface, error);
        if (!hostapd || !hostapd->wait_enabled(20000, error)) {
            log("hostapd: {}", error);
            fs::remove(cred_path);
            return 1;
        }

        const PairingCode code = pairing_code_for(rec.ap_mac);
        const auto armed = hostapd->command("WPS_PIN any " + code.wps_pin() + " 600");
        if (!armed || armed->rfind("FAIL", 0) == 0) {
            log("hostapd refused the WPS PIN: {}", armed.value_or("(no reply)"));
            fs::remove(cred_path);
            return 1;
        }

        std::fprintf(stderr,
            "\n"
            "  ============================================================\n"
            "   1. Turn the GamePad on and press its SYNC button until\n"
            "      the pairing screen appears.\n"
            "   2. Enter these symbols on the GamePad, in this order:\n"
            "\n"
            "          %s      (%s)\n"
            "\n"
            "   Waiting up to 10 minutes. Ctrl+C cancels.\n"
            "  ============================================================\n\n",
            code.symbols().c_str(), code.names().c_str());

        const int64_t deadline = monotonic_us() + 600LL * 1000000;
        int64_t success_at = 0;
        while (monotonic_us() < deadline) {
            pollfd fds[] = {{signal_fd, POLLIN, 0}, {hostapd->output_fd(), POLLIN, 0}, {hostapd->event_fd(), POLLIN, 0}};
            ::poll(fds, std::size(fds), 200);
            if (fds[0].revents & POLLIN) {
                log("pairing cancelled");
                break;
            }
            if ((fds[1].revents & (POLLIN | POLLHUP)) && !hostapd->drain_output()) {
                log("hostapd exited unexpectedly");
                break;
            }
            while (auto ev = hostapd->read_event()) {
                log("event: {}", *ev);
                if (auto pad = mac_after(*ev, "WPS-REG-SUCCESS ")) {
                    rec.gamepad_mac = pad;
                    success_at = monotonic_us();
                } else if (ev->rfind("WPS-FAIL", 0) == 0) {
                    log("WPS failed. Check the symbols and try again (press SYNC on the GamePad).");
                }
            }
            // Give the GamePad a moment to finish the WPS exchange (M8 + EAP
            // failure/done) before we take the pairing AP down.
            if (success_at && monotonic_us() - success_at > 3000000) {
                paired = true;
                break;
            }
        }
    }
    fs::remove(cred_path);
    if (!paired)
        return 1;

    if (!save_pairing(o.store, rec, error)) {
        log("pairing worked, but saving failed: {}", error);
        return 1;
    }
    log(">>> paired with GamePad {}. Saved to {}", rec.gamepad_mac->to_string(), o.store);
    if (o.no_run)
        return 0;
    log("switching to the normal access point...");
    return cmd_run(o, signal_fd);
}

// ---------------------------------------------------------------- others

int cmd_clock_probe(const Options& o, int signal_fd)
{
    auto clock = open_clock(o.iface);
    if (!clock)
        return 1;
    const int64_t end = monotonic_us() + int64_t(o.seconds) * 1000000;
    int ok = 0, bad = 0;
    while (monotonic_us() < end) {
        if (clock->sample())
            ++ok;
        else {
            ++bad;
            log("sample rejected: {}", clock->last_error());
        }
        pollfd p{signal_fd, POLLIN, 0};
        if (::poll(&p, 1, 100) > 0)
            break;
    }
    log("samples: {} good, {} rejected; healthy={}; TSF={} us; drift {:+.2f} ppm vs CLOCK_MONOTONIC",
        ok, bad, clock->healthy(), clock->now_tsf(), clock->drift_ppm().value_or(0));
    log("expected: almost all good, and drift within about ±50 ppm.");
    return bad * 10 > ok ? 1 : 0;
}

int cmd_status(const Options& o)
{
    const auto rec = load_pairing(o.store);
    if (!rec) {
        log("no pairing stored in {}", o.store);
        return 1;
    }
    log("AP MAC {}  SSID {}  channel {}  GamePad {}", rec->ap_mac.to_string(), rec->ssid, rec->channel,
        rec->gamepad_mac ? rec->gamepad_mac->to_string() : "(not seen yet)");
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const auto opts = parse_args(argc, argv);
    if (!opts) {
        usage();
        return 2;
    }
    if (opts->command == "status")
        return cmd_status(*opts);
    if (!preflight(*opts))
        return 1;
    const UniqueFd signals = make_signal_fd();
    try {
        if (opts->command == "pair") return cmd_pair(*opts, signals.get());
        if (opts->command == "run") return cmd_run(*opts, signals.get());
        if (opts->command == "clock-probe") return cmd_clock_probe(*opts, signals.get());
    } catch (const std::exception& e) {
        log("fatal: {}", e.what());
        return 1;
    }
    usage();
    return 2;
}
