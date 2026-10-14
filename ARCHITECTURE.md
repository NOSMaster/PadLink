# padlink architecture

## 1. Overview

```
 ┌────────────── your app (emulator / recomp runtime / example) ──────────────┐
 │  libpadlink client API  include/padlink/padlink.h                          │
 └──────────────────┬──────────────────────────────────────────────────────────┘
                    │ Unix socket /run/padlink/padlink.sock (src/api/wire.h: length-prefixed messages)
 ┌──────────────────▼──────────────────── padlinkd (runs as root) ────────────┐
 │  Session manager ── state machine: Idle → Pairing → Connecting → Active    │
 │   ├─ wifi/   hostapd supervisor (ctrl socket), DHCP server, pairing store  │
 │   ├─ clock/  MediaClock (AP TSF backends, health checks)                   │
 │   ├─ net/    UDP sockets 500x0..500x5 bound to 192.168.1.10, QoS TID 5     │
 │   ├─ cmd/    CMD state machine (UIC read, UVC/UAC keepalive, ACKs)         │
 │   ├─ input/  HID decode → uinput virtual gamepad + touch + motion devices  │
 │   ├─ video/  frame source → encoder → VSTRM packetizer → paced sender      │
 │   └─ audio/  PCM ring buffer → ASTRM packetizer → 8.67 ms sender           │
 │  protocol/   pure parse/build functions, no I/O  ← unit tests live here    │
 └──────────────────┬─────────────────────────────────────────────────────────┘
                    │ spawns
          patched hostapd 2.11 (our patch series) ── nl80211 ── rtw89 ── 📶 GamePad
```

`scripts/padlink-net-up.sh` and `scripts/padlink-net-restore.sh` handle
everything that touches the system's normal networking, so it can always be
undone.

## 2. Source layout

```
padlink/
  include/padlink/   public API (libpadlink)
  src/protocol/      pure parse/build code: input, VSTRM, ASTRM, CMD, DHCP, pairing, WPS credential
  src/api/           app socket protocol (wire) and padlinkd's API server
  src/client/        libpadlink (the app side), RGBA -> I420 conversion
  src/clock/         AP TSF media clock
  src/net/           the console-side UDP sockets
  src/input/         uinput virtual gamepad, touchscreen and motion devices
  src/video/         frame sources, x264 encoder, H.264 header checks, paced sender
  src/audio/         audio sources (tone, FIFO) and paced sender
  src/wifi/          hostapd supervisor and config, DHCP server
  src/store/         the pairing record
  src/daemon/        padlinkd
  tests/             unit tests (doctest)
  tools/             video self-test and file encoder
  examples/          demo app
  scripts/           net-up / net-restore / audio sink
  third_party/       hostapd and x264 build scripts with our patches, doctest
```

## 3. Key design rules

- **The protocol layer is pure.** Functions take bytes and return structs (or
  the other way round) using `std::span<const uint8_t>`. They never touch
  sockets or time, so they are tested with hand-written byte arrays.
- **Explicit byte order.** No `#pragma pack` structs cast over buffers.
  Fields are read with small `read_be16()`/`read_le16()` helpers, which avoids
  the bit-field portability bugs vanilla works around with `reverse_bits`.
- **RAII for every resource.** Each socket, file descriptor, child process
  and uinput device is owned by an object whose destructor releases it. If
  padlinkd crashes, hostapd is still killed, and the restore script cleans up
  anything that's left.
- **Clock safety.** Media is only sent while `MediaClock::healthy()`. A frame
  with a wrong timestamp is worse than no frame.
- **Threads.** One receive thread (`poll()` over all sockets, CMD timers),
  one video-pacing thread (`clock_nanosleep` to chunk deadlines), one audio
  thread. Threads share data through small mutex-protected queues, with no
  global state.

## 4. Wi-Fi stack choice

We don't install hostapd system-wide. CMake builds upstream **hostapd 2.11**
(BSD-3-Clause) into `build/` and applies **our own** patch series behind
`CONFIG_PADLINK_DRC`:

1. RSN: Nintendo `a4:c0:e1` AKM/cipher selectors on air; standard selectors
   passed to nl80211.
2. WPA: rotate the PTK left by 3 bytes.
3. WPS: rotate the KDK left by 3 bytes; Nintendo WPS vendor type; WPS 1.0
   IEs; Wii U IE order.
4. rtw89 robustness: retry/defer GTK, retry PTK, re-assert station flags
   (PROTOCOL.md §1.3).

Barista's GPL patch series showed that this approach works on the
RTL8852BE. We used it as a reference for *what* to change and wrote the code
ourselves.

## 5. Networking

While padlink runs, the Wi-Fi interface is taken away from NetworkManager,
so it can't be used for anything else. `padlink-net-restore.sh` gives it
back. Ethernet or a second Wi-Fi adapter keeps the PC online meanwhile.

Cards that support `#{managed} + #{AP}` on one channel could in principle run
the GamePad AP next to the normal connection, but airtime sharing would hurt
video and rtw89 concurrency is untested, so padlink uses the card exclusively.

## 6. Privileges

padlinkd needs root, or `CAP_NET_ADMIN` + `CAP_NET_RAW`, plus access to
`/dev/uinput` and debugfs. Apps never run as root: they reach the daemon
through `/run/padlink/padlink.sock`, which belongs to the user who started
padlinkd with sudo. padlinkd is started by hand; a systemd unit and a polkit
rule may come later.

## 7. License

GPL-2.0-or-later, compatible with every reference (Barista and vanilla
GPL-2.0, drc-sim GPL-2.0+, libdrc BSD-2-Clause, hostapd BSD-3-Clause) and
with x264. Apps that link libpadlink must be GPL-compatible; an app that only
talks to padlinkd over the socket protocol is a separate program.

