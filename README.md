# PadLink

**Use a real Wii U GamePad with a Linux PC: no Wii U needed.**

PadLink makes your PC act as the Wii U console. The GamePad connects to it over
Wi-Fi exactly as it would to a console. You can then:

- **use it as a controller**: it appears as a normal Linux gamepad (Steam,
  Big Picture and SDL games just work), with its touchscreen and motion sensors
  as extra devices;
-  **stream to its screen**: your desktop for the moment (Still in developement, can be rough in quality)

```
GamePad ⟵ 5 GHz Wi-Fi ⟶ PC (padlinkd + patched hostapd) ⟵ socket ⟶ Usage
```

> Status: everything above works on real hardware. Tested with one Wi-Fi card
> so far (see [Hardware](#hardware)). Feedback from other cards is welcome.

---

## Contents

- [Hardware](#hardware)
- [Build](#build)
- [Pairing and connecting](#pairing-and-connecting)
- [The GamePad as a controller](#the-gamepad-as-a-controller)
- [Video: your screen on the GamePad](#video-your-screen-on-the-gamepad)
- [Audio and rumble](#audio-and-rumble)
- [For developers: libpadlink](#for-developers-libpadlink)
- [How it works](#how-it-works)
- [Troubleshooting](#troubleshooting)
- [Credits and license](#credits-and-license)

---

## Hardware

You need a **Wi-Fi card that can run a 5 GHz access point** with a
**SoftMAC (mac80211) Linux driver**, where Linux handles the WPA handshake
itself. FullMAC cards, which do the handshake in their firmware, can't work:
the GamePad uses a slightly modified WPA2 that has to be done in software.
**2.4GHz wifi cards and amongst others cards such as Intel and probably Broadcom is and WILL never be supported due to the limitations of those cards**
In theory, others Realtek chips should be compatible along with some Mediatek and Atheros one but not CONFIRMED AT THE MOMENT.
| Card | Driver | Status |
|---|---|---|
| Realtek RTL8852BE | `rtw89_8852be` | works (developed and tested on it) |
| other cards | | untested: please report! |

The media clock currently reads the access point timer through rtw89's
debugfs, so **other drivers will need a small clock backend**
(`src/clock/`). The rest is driver-independent.

While PadLink runs, **the Wi-Fi card is used for the GamePad**, so your normal
Wi-Fi is off (use Ethernet, or a second card, if you need internet). Nothing is
changed permanently: one script, or a reboot, brings everything back.

## Build

Needs CMake ≥ 3.24, GCC ≥ 13, git, make, libnl-3 and OpenSSL. Optional:
nasm (a much faster video encoder) and FFmpeg (for the video self-test).

```sh
git clone <this repository> padlink && cd padlink
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

This builds:

| File | What it is |
|---|---|
| `build/padlinkd` | the daemon (plays the console's role) |
| `build/padlink-hostapd` | hostapd 2.11 with our patches (`third_party/hostapd/patches`) |
| `build/libpadlink.a` + `include/padlink/padlink.h` | the app library |
| `build/padlink-demo` | a demo app |

x264 is built from source with one small patch of ours. Nothing is installed
system-wide.

## Pairing and connecting

⚠ **This takes your Wi-Fi card away from NetworkManager: your internet goes
down** until you restore it with `sudo scripts/padlink-net-restore.sh` (a
reboot does the same).

**First time: pair the GamePad**

```sh
sudo scripts/padlink-net-up.sh wlan0     # free wlan0 for PadLink (temporary firewall rules too)
sudo build/padlinkd pair                 # shows 4 symbols
#   on the GamePad: press SYNC, then enter the 4 symbols
#   once paired it connects right away; Ctrl+C to stop
sudo scripts/padlink-net-restore.sh      # normal Wi-Fi again
```

**Every other time**

```sh
sudo scripts/padlink-net-up.sh wlan0
sudo build/padlinkd run                  # then turn the GamePad on
```

Replace `wlan0` with your card's interface (`ip link` lists them). Other
commands: `sudo build/padlinkd status` and `sudo build/padlinkd clock-probe`.

## The GamePad as a controller

While `padlinkd run` is running, Linux sees three devices:

| Device | What it reports |
|---|---|
| `Wii U GamePad (padlink)` | buttons (Nintendo layout: B is south, A is east), D-pad, both sticks |
| `Wii U GamePad (padlink) Touchscreen` | touch position (854×480 pixels) and pressure |
| `Wii U GamePad (padlink) Motion` | accelerometer and gyroscope |

`--verbose-input` logs every change with raw values; `--no-uinput` turns the
devices off.

## Video: your screen on the GamePad

By default the GamePad shows a test pattern. To show your desktop (Wayland),
pipe frames in with `wf-recorder`. It runs as **your user**; only padlinkd
needs root:

```sh
wf-recorder -y -m rawvideo -c rawvideo -x yuv420p -F "scale=854:480,pad=864:480" -f /dev/stdout \
  | sudo build/padlinkd run --video stdin
```

`--video none` turns video off. The video is H.264 at 854×480. The test
suite includes an offline check that encodes a frame exactly as sent to the
GamePad and makes FFmpeg decode it strictly.

## Audio and rumble

```sh
sudo build/padlinkd run --audio tone          # a beep every second
sudo build/padlinkd run --rumble-test         # the rumble motor pulses every 3 s
```

**Desktop sound on the GamePad:** start padlinkd with `--audio fifo`, then in
another terminal, as your user:

```sh
scripts/padlink-audio.sh            # adds a "Wii-U-GamePad" output; pick it per app
scripts/padlink-audio.sh --default  # ...and make it the default output
scripts/padlink-audio.sh --mirror   # hear your sound on the PC and the GamePad
```

Ctrl+C removes the output again. Video and sound together:

```sh
wf-recorder -y -m rawvideo -c rawvideo -x yuv420p -F "scale=854:480,pad=864:480" -f /dev/stdout \
  | sudo build/padlinkd run --video stdin --audio fifo
```

**Try the demo:**

```sh
sudo build/padlinkd run        # terminal 1
build/padlink-demo             # terminal 2, as your user
```

Move the square with the left stick, draw by touching the screen (X clears),
watch the boxes light up per button, hold A to rumble and B for a tone.

## How it works

The GamePad talks to the console over WPA2 Wi-Fi on 5 GHz with two small
twists. Nintendo rotates two keys by 3 bytes: one in WPS pairing and one in
the normal handshake. Our hostapd patches implement both; everything else is
standard. On top of that run a handful of UDP streams: input reports, H.264
video, PCM audio and a command channel. All of them are timed against the
access point's hardware clock, which the GamePad expects.

- [PROTOCOL.md](PROTOCOL.md): everything we know about the protocol, with
  how sure we are of each point.
- [ARCHITECTURE.md](ARCHITECTURE.md): how padlinkd is organised.

The protocol code is pure (bytes in, structs out) and covered by unit tests.

## AI Usage
AI (Claude Code) was used during the making of this project to help make progress much quicker and for debugging

## Credits and license

PadLink is an independent implementation, **GPL-2.0-or-later** (see
[LICENSE](LICENSE)). It owes a lot to the people who documented the GamePad
before: [Barista](https://github.com/BetaZay/Barista),
[vanilla](https://github.com/vanilla-wiiu/vanilla),
[libdrc](https://github.com/rolandoislas/libdrc) and
[drc-sim](https://github.com/rolandoislas/drc-sim). It uses hostapd and
x264. [CREDITS.md](CREDITS.md) lists what came from where.

No Nintendo firmware, keys, certificates or other copyrighted material is in
this repository. Wii U and Wii U GamePad are trademarks of Nintendo; this
project is not affiliated with or endorsed by Nintendo.
