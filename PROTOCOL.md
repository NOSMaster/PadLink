# padlink — Wii U GamePad protocol notes

These notes describe the protocol from the **console side**. In this project
the PC plays the console, which the references call the **DRH**; the GamePad
is the **DRC**. They come from reading the reference projects (see
[CREDITS.md](CREDITS.md)) and are written in our own words. Nothing here comes
from Nintendo firmware, and the protocol needs no Nintendo key: every secret
used during a session (the PSK) is one that **we generate**.

Each fact carries a confidence tag:

- **[C] Confirmed**: two or more references agree, or one reference checked it
  against a real console capture.
- **[O] Observed**: seen in a capture or used by one working implementation,
  but nobody has fully explained it.
- **[?] Open**: references disagree or nobody knows yet.

Byte offsets start at 0. "BE" means big-endian (most significant byte first,
the usual "network order"). "LE" means little-endian (what x86 uses). The
protocol mixes both, sometimes inside one packet, so every field below gives
its byte order.

---

## 1. Radio layer

### 1.1 Requirements

- 5 GHz 802.11n, HT20. **[C]**
- A **SoftMAC** card, where the Linux CPU builds and encrypts the frames
  (mac80211 drivers). On FullMAC cards the firmware does the WPA handshake
  itself, and we can't apply Nintendo's tweaks there. **[C]**
- Your RTL8852BE (`rtw89_8852be`) is mac80211/SoftMAC. Vanilla's compatibility
  list marks it as working, and Barista was developed on it. **[C]**

### 1.2 The obfuscations (only two small ones)

1. **PTK rotation (normal WPA2 sessions).** In a normal 4-way handshake both
   sides derive the PTK (Pairwise Transient Key) from the PSK and the two
   nonces. Nintendo then **rotates the PTK left by 3 bytes**: the first 3
   bytes move to the end. Encryption is still standard AES-CCMP. **[C]**
2. **KDK rotation (WPS pairing).** In WPS, the AuthKey/KeyWrapKey/EMSK are
   derived from a KDK. Nintendo rotates **the KDK left by 3 bytes** before
   running that derivation. A standard WPS stack fails at message M2. **[C]**

Beacons also advertise **Nintendo vendor RSN selectors** (OUI `a4:c0:e1`)
instead of the standard `00:0f:ac`:

| RSN field | Standard | Wii U |
|---|---|---|
| Pairwise cipher | `00:0f:ac:4` (CCMP) | `a4:c0:e1:4` **[C]** |
| Group cipher | `00:0f:ac:4` | `a4:c0:e1:4` **[C]** |
| AKM | `00:0f:ac:2` (PSK) | `a4:c0:e1:2` **[C]** |

If the selectors don't match, the GamePad refuses to associate (libdrc
reports error `0x000c`).

**Linux gotcha [C]:** the kernel (nl80211) only accepts the *standard*
selectors when hostapd installs keys or starts the AP. A patched hostapd has
to put the Nintendo selectors **in the over-the-air IEs** but pass **standard
CCMP/PSK selectors to the driver**. Barista's patches 0004 and 0009 do this.

### 1.3 rtw89-specific issues (from Barista's hostapd patches) **[O]**

These are worth knowing, because they show up as "the GamePad associates and
then drops":

- rtw89 can return `ENOENT` when hostapd installs the first group key (GTK)
  right after `START_AP`. Fix: retry it, or defer the GTK until the first
  pairwise key succeeds.
- The GamePad reassociates immediately after boot and can finish the 4-way
  handshake before mac80211 is ready for per-station keys, so `NEW_KEY`
  returns `ENOENT`. Fix: retry the PTK for a short, bounded time.
- After the association-response ACK, re-assert the station's
  AUTHENTICATED/ASSOCIATED flags, or mac80211 rejects the pairwise key.
- NetworkManager must fully release the interface first. Barista waits about
  5 s after `nmcli dev set wlan0 managed no` on some adapters.

We'll write our **own** hostapd patch series against upstream hostapd 2.11
(BSD-licensed) that implements these behaviors. See ARCHITECTURE.md §4.

### 1.4 Pairing (WPS), with the PC as the console

The real console flow: you press SYNC on the console, the TV shows 4 card
symbols, you press SYNC on the GamePad, then you enter the symbols on the
GamePad. Over the air it works like this:

1. **Pairing AP (open, WPS registrar).** **[C]**
   - SSID: `WiiU<first 11 hex nibbles of M><all 12 nibbles of M>_STA1`, where
     `M` is the AP MAC with its **last byte replaced by the PIN byte**
     (Barista, from a real capture) **[O]**. Vanilla, as a client, only checks
     that the name contains `WiiU` and `_STA1`. **[C]**
   - WPS IEs use **Nintendo vendor type `a4:c0:e1:f4`** and WPS 1.0 (no WFA
     WPS 2.0 extension). The advertised config methods keep the push-button
     bit. **[O]**
   - IE order matters while the GamePad picks a pairing AP: Broadcom vendor HT
     IE, then the Nintendo WPS IE, then the Broadcom compatibility IE, then
     WMM. **[O]**
   - Device name `WiiU<ap-mac-hex>`, manufacturer "Broadcom", model "SoftAP".
     **[O]** These are plain strings, not secrets.
2. **PIN.** The 4 symbols are digits 0–3: ♠=0, ♥=1, ♦=2, ♣=3. The 8-digit WPS
   PIN is `<4 symbol digits>` followed by the fixed `5678`. Example: ♠♠♦♣ →
   `00235678`. **[C]** As the console, **we pick the symbols** and print them;
   you tap the same ones on the GamePad. Barista packs the 4 digits into one
   byte (2 bits each), which also becomes the last byte of the SSID MAC
   above. **[O]**
3. **Credential handed over by WPS (M8).** We build it ourselves. **[C]**
   - SSID of the normal-mode AP (Barista uses `WiiU<ap-mac-hex>`)
   - Auth `WPA2-PSK` (0x0020), encryption `AES` (0x0008)
   - Network key: **64 hex characters** (a raw 256-bit PSK we generate at
     random)
   - MAC address of the AP
4. Afterwards the GamePad stores the credential, and pairing mode ends.

**What padlink stores to "remember" a GamePad:** AP MAC, runtime SSID, PSK,
channel, and the GamePad's MAC. All of it is generated by us.

### 1.5 Normal (runtime) AP **[C]**

- WPA2-PSK with the Nintendo selectors and PTK rotation (§1.2), CCMP, using
  the PSK from pairing.
- SSID hidden: Barista sets `ignore_broadcast_ssid=2`, which keeps the SSID's
  length but zeroes its bytes. libdrc says the real console beacons an empty
  SSID.
- Channel: Barista tries 36/40/44/48/149–165 (non-DFS). 36–48 are allowed in
  most countries; 149–165 are restricted in some (e.g. much of Europe).
- WMM enabled. Barista copies the real console's EDCA parameters into the
  config.

### 1.6 IP layer **[C]**

| | Address |
|---|---|
| Console (PC) | `192.168.1.10/24` |
| GamePad 1 | `192.168.1.11` (given by DHCP) |
| GamePad 2 | `192.168.1.12` (not supported by padlink v1) |
| Interface MTU | `1800` |

A small built-in DHCP server answers the GamePad: lease 3600 s, mask
255.255.255.0, router and server ID 192.168.1.10, broadcast 192.168.1.255,
**option 26 (MTU) = 1800**. **[O]** (These are Barista's values.)

> ⚠ The GamePad network is 192.168.1.0/24. If your home LAN also uses
> 192.168.1.x, routing will conflict. Your Wi-Fi is taken over during
> sessions anyway (ARCHITECTURE.md §5), so the conflict only matters with
> Ethernet or a second adapter.

---

## 2. UDP services

Every GamePad-side port is the console port **+100**. **[C]**

| Console port | Pad port | Name | Direction (main use) |
|---|---|---|---|
| 50010 | 50110 | MSG | Pad → console (IDR requests); console broadcast |
| 50020 | 50120 | **VSTRM** video | console → pad (pad → console = camera, ignored) |
| 50021 | 50121 | **ASTRM** audio + video-format | console → pad (pad → console = mic) |
| 50022 | 50122 | **HID** input | pad → console |
| 50023 | 50123 | **CMD** RPC | both |
| 50025 | 50125 | unknown | console → pad **[?]** |

QoS **[O]**: a real console sends video and audio as 802.11 **QoS TID 5** with
RTS/CTS protection at about HT MCS 6. Barista gets TID 5 by setting
`SO_PRIORITY=5` and `IP_TOS=0xa0` on the media sockets.

---

## 3. HID input (pad → console 50022)

One **128-byte** datagram, about **180 times per second**. Any other size is
discarded. If nothing arrives for 1/60 s, libdrc treats the input as lost.
**[C]**

| Offset | Size | Encoding | Field |
|---|---|---|---|
| 0 | 2 | BE u16 | sequence number |
| 2 | 2 | **BE** u16 | buttons (table below) |
| 4 | 1 | bits | power status: `0x01` AC plugged in, `0x02` power button pressed, `0x40` charging, `0x80` USB power |
| 5 | 1 | u8 | battery charge |
| 6 | 2 | **LE** u16 | left stick X |
| 8 | 2 | LE u16 | left stick Y |
| 10 | 2 | LE u16 | right stick X |
| 12 | 2 | LE u16 | right stick Y |
| 14 | 1 | u8 | volume slider |
| 15 | 6 | 3× LE s16 | accelerometer **[?]** axis order (libdrc: x,y,z; vanilla: z,x,y) |
| 21 | 9 | 3× LE s24 | gyroscope **[?]** axis order (libdrc: roll,yaw,pitch; vanilla: roll,pitch,yaw) |
| 30 | 6 | ? | magnetometer (format unknown) |
| 36 | 40 | 10× point | touchscreen samples (§3.2) |
| 76 | 4 | ? | unknown |
| 80 | 1 | bits | extra buttons: `0x80` L3, `0x40` R3, `0x20` TV |
| 81 | 46 | ? | unknown. **Byte 83 bit 0 = "waiting for video"** **[O]** (Barista uses it as a "session active" signal) |
| 127 | 1 | u8 | bitwise NOT of the UIC firmware version |

### 3.1 Buttons (offset 2, BE u16) **[C]**

Verified on real hardware on 2026-10-13: A, B, X, Y, Up and Down gave exactly
these bits, and input arrived at a steady 180 reports/s.

```
0x8000 A      0x4000 B      0x2000 X      0x1000 Y
0x0800 Left   0x0400 Right  0x0200 Up     0x0100 Down
0x0080 ZL     0x0040 ZR     0x0020 L      0x0010 R
0x0008 Plus   0x0004 Minus  0x0002 Home   0x0001 Sync
```

libdrc and Barista combine these into one 24-bit value:
`(byte80 << 16) | (byte2 << 8) | byte3`. That gives TV=0x200000, R3=0x400000,
L3=0x800000.

### 3.2 Sticks and touch

- Sticks: raw values roughly **900–3200**, center about 2048. Clamp, center,
  then normalize to −1..1. Y may need flipping for Linux conventions. **[O]**
- Touch: each of the 10 points is two LE u16 values (x, then y). The **low 12
  bits** are the coordinate. **Bits 12–14** carry 3 "extra" bits:
  - pressure = the extra bits of point0.x, point0.y, point1.x and point1.y
    joined into 12 bits (non-zero means touched) **[C]**
  - firmware version bits sit in points 6–8 **[O]**
- **Average the 10 samples** to cut noise. **[C]**
- Raw to pixels uses 2-point calibration. The defaults the pad uses before it
  loads its own calibration: raw (195, 3818) → screen (20, 20) and raw
  (3877, 373) → screen (834, 460). So **raw Y is inverted**. **[C]** The
  GamePad's real calibration is in its UIC config (§6.3).

---

## 4. VSTRM video (console 50020 → pad 50120)

### 4.1 Picture format

- **H.264**, coded **864×480** (54×30 macroblocks); the visible area is
  **854×480** (the decoder crops 10 px on the right). **[C]**
- 59.94 Hz nominal. **[C]**
- Each frame is cut into **5 chunks** of **6 macroblock rows** (96 px) each.
  **[C]**
- **No SPS, PPS, slice header or Annex-B start codes are sent.** The GamePad
  has a fixed decoder configuration and builds the headers itself. The UDP
  payload is the raw slice data. **[C]** The bitstream therefore has to match
  the GamePad's built-in assumptions. What references use or found:
  CABAC; no B-frames; 1 reference frame; no 8×8 transform; no weighted
  prediction; constrained intra; fixed QP (32 in both libdrc and Barista);
  **no I16x16 planar prediction** (Barista found planar risky); one slice per
  frame. **[O]**
- Reconstructed headers (used by vanilla and by Barista's analysis tools,
  useful for our unit tests): IDR slice header word `0x25b804ff`, P slice
  header word `0x21e003ff | ((frame_num & 0xff) << 13)`. **[O]**
- **GamePad decoder configuration [C]** (decoded from the SPS/PPS bytes in
  libdrc: SPS `67 64 00 20 ac 2b 40 6c 1e f3 68`, PPS `68 ee 06 0c e8`):
  High profile, level 3.2, 54×30 MBs with 10 px right crop, frame_num 8 bits,
  POC type 2, 1 reference, **CABAC**, **pic_init_qp 32** (slice QP delta 0 in
  the fixed header ⇒ QP 32 everywhere unless mb_qp_delta is used),
  **constrained_intra_pred = 1**, no 8×8 transform, chroma QP offset 0,
  deblocking on with default offsets.
- **One slice per frame [C]:** the GamePad (and vanilla) concatenates all
  chunk payloads into one slice. The chunks are byte ranges of a single
  continuous CABAC stream, and Barista places each boundary ~2 bytes *after*
  the row group's data, so the decoder reads across chunks. padlink v1 uses
  x264 and puts every boundary "late" (nearly all bytes in chunk 0), which
  can't end a chunk too early.
- **How CABAC is handled at chunk boundaries** (solved in padlink: our x264
  patch reports where each group of 6 macroblock rows ends in the CABAC
  stream, and chunks are cut there; see `src/video/encoder.cpp`).
  libdrc needed a **patched x264** ("DRH mode"); Barista wrote its own
  encoder on top of minih264 with a "continuous CABAC sink". A stock encoder
  will not work unmodified. This is the biggest technical risk in the
  project.

### 4.2 Packet header (16 bytes, then up to 1694 payload bytes)

The first 4 bytes are one **BE 32-bit word**, read from the most significant
bit down: **[C]**

```
bits 31..28  magic = 0xF
bits 27..26  packet type (0)
bits 25..16  sequence number (10 bits, modulo 1024, +1 per packet)
bit  15      init         (first frame of the stream; also used on recovery IDRs [O])
bit  14      frame_begin  (first packet of a frame)
bit  13      chunk_end    (last packet of one of the 5 chunks)
bit  12      frame_end    (last packet of chunk 4)
bit  11      has_timestamp (always 1)
bits 10..0   payload size (11 bits; must equal UDP length − 16)
```

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | the word above |
| 4 | 4 | **BE** u32 timestamp (µs, AP TSF clock, §7). Same value in every packet of a frame. |
| 8 | 8 | extension options, then zero padding |
| 16 | ≤1694 | slice bytes |

**Max payload [C]:** a real console caps it at **1694** bytes. libdrc used
1400. Barista moved to 1694 after comparing against a real capture.

### 4.3 Extension options

Options are single bytes; some take a 1-byte argument. They're read until a
`0x00` byte or the end of the 8 bytes. **[C]**

| Byte | Arg | Meaning |
|---|---|---|
| `0x80` | – | this frame is an IDR |
| `0x81` | 1 | unused |
| `0x82` | 1 | frame rate: 0=59.94, 1=50, 2=29.97, 3=25 |
| `0x83` | – | force decoding |
| `0x84` | – | clear force decoding |
| `0x85` | 1 | macroblock rows per chunk (6) |

Byte order used by a real console: **[O]**

```
IDR: 80 82 00 83 85 06 00 00
P  : 82 00 83 85 06 00 00 00
```

### 4.4 Timing and recovery **[O]** (Barista's measurements)

- Before **each** frame, send a **video-format** packet on ASTRM (§5.3) with
  the same timestamp.
- Barista stamps the format with `AP_TSF − 1250 µs` and sends the video
  about 5 ms later (the real console's lead is about 5.1 ms).
- Spread the packets of a frame over about 13 ms instead of sending them in
  one burst. Approximate deadlines for the last packet of each chunk:
  chunk 0 → 2.5 ms, 1 → 5, 2 → 7.5, 3 → 10, 4 → 13 ms.
- After an IDR, the real console sends: format + IDR, then **one slot with
  only a format packet**, then format + P.
- The GamePad asks for a new IDR with a MSG packet (§8). Barista also sends an
  IDR every 60 frames anyway, because corruption then clears instead of
  lingering.

### 4.5 Hardware results (padlink)

| Date | Setup | Result |
|---|---|---|
| 2026-10-13 | x264 superfast, sub-pixel MVs, late chunk split, IDR every 60 + on request | **Picture shows.** Green ghosting on moving objects, colors bleeding, stutter; ~5–30 IDR requests/s. Offline FFmpeg decode of the same bitstream is clean (47.7 dB), so the GamePad decodes some inter-prediction feature differently. Matches Barista's finding that restricting motion reduces corruption. |
| 2026-10-13 | + ignore duplicate IDR requests (3-frame holdoff), MONOTONIC_RAW clock | IDR requests drop to a steady **2.0/s**, and every IDR answers a request. Clock drift ±10 ppm. |
| 2026-10-13 | `--video-mode fullpel` / `--chunk-split even` | Same 2.0/s; no difference in request rate. |
| 2026-10-13 | `--video-mode intra` (IDR every frame, **no** gap slot) | A request after almost every frame (~35/s); the GamePad disconnected after 4 s. Back-to-back IDRs without the gap slot are not accepted. |
| 2026-10-13 | baseline + request timing | Requests come in pairs, **40 ms and 50 ms after an IDR we sent on our own** (the 60-frame periodic one), i.e. right after the first P frame that follows it. We answer with another IDR, which is accepted: silence for the next ~60 frames. Period 1.08 s = 65 slots. |
| 2026-10-13 | `--ignore-idr-requests` | The GamePad requests after **every** frame, forever. The periodic IDRs do not end it. |
| 2026-10-13 | init flag on every IDR | Same request pattern; **visually the best** (user). Now the default. |
| 2026-10-13 | intra (all IDRs) with the gap slot | "really bad" (user). Removed. |

| 2026-10-13 | `--idr-every 0` (IDR only at start and on request) | **Clean, steady stream**, only 1 IDR in 20 s. Now the default. |
| 2026-10-13 | no format-only gap slot after an IDR | Nothing is displayed; IDR requests pile up. **The gap slot is required.** |

| 2026-10-13 | real desktop (wf-recorder), late split | Picture visible but **green tint over the lower ~75–80%**. The 71 KB IDR was all in chunk 0. Offline decode is clean, so the GamePad needs each chunk to hold its own 6 MB rows. Fixed by cutting at the real row ends (patched x264) + 2 bytes slack, like a real console. |

| 2026-10-13 | real desktop, row-exact chunk split | **No change**: still green below the top band (photo), blocky edges, luma sharp, so the chunk split was not the cause. The top MB row can't use plane prediction, so the suspect is that the GamePad decodes intra **plane** prediction wrongly (Barista also disables it). x264 patched to never use plane. |
| 2026-10-13 | + no plane prediction (luma 16x16 and chroma) | **Fixed: correct colours everywhere, no issues** (user). |

**Rules that work:** never use intra plane prediction (16x16 luma or chroma),
because the GamePad decodes it differently from the standard; cut chunks at
the real 6-MB-row ends + 2 bytes; IDR only at stream start and when the
GamePad asks (MSG `01 00 00 00`), ignoring repeats for 3 frames; the init flag on every IDR; one
format-only slot after each IDR; never send unrequested periodic IDRs, because
the GamePad rejects the P frame that follows them.

---

## 5. ASTRM audio and video format (console 50021 → pad 50121)

### 5.1 Header (8 bytes)

The first 2 bytes are one **BE 16-bit word**: **[C]**

```
bits 15..13  format: 0=PCM 24k, 1=PCM 48k, 2=A-law 24k, 3=A-law 48k, 4=µ-law 24k, 5=µ-law 48k
bit  12      mono
bit  11      vibrate (rumble while set)
bit  10      type: 0 = audio samples, 1 = video format
bits  9..0   sequence (10 bits)
```

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | word above |
| 2 | 2 | **BE** u16 payload length |
| 4 | 4 | **LE** u32 timestamp **[O]** (Barista read it from a capture; libdrc's docs give no byte order) |

### 5.2 PCM audio **[O]**

- Format 1 (48 kHz), stereo, s16 LE interleaved.
- **416 frames = 1664 bytes** per packet, so one packet every **8.667 ms**.
- Example: seq 0x321, ts 0x12345678, no rumble → `23 21 06 80 78 56 34 12`.
- padlink: sequence counts PCM packets only (the video-format messages keep
  sequence 0); the timestamp is the AP TSF when the packet is sent. Rumble =
  the vibrate bit on PCM packets, so padlink keeps sending (silent) PCM
  whenever audio or rumble is enabled.

### 5.3 Video-format message (32 bytes, one before every video frame)

```
04 00 00 00  10 10 81 08  [ts LE]  80 3e 00 00  80 3e 00 00  08 52 00 00  08 52 00 00  00 00 00 00
```

| Offset | Value | Meaning |
|---|---|---|
| 0 | `0x04` | type bit set (byte 0 bit 2 = header bit 10) |
| 2–3 | `00 00` | length deliberately 0 (real console does this) **[O]** |
| 4–7 | `10 10 81 08` / `11 11 81 08` / `11 11 81 88` | **[?]** varies between captures |
| 8–11 | LE u32 | timestamp = the timestamp of the matching VSTRM frame |
| 12–19 | 2× LE u32 16000 | clock/delay fields **[O]** |
| 20–27 | 2× LE u32 21000 | **[?]** |
| 28–31 | 0 | video format / padding |

---

## 6. CMD RPC (50023 ↔ 50123)

### 6.1 Transport header (8 bytes, all **LE** u16) **[C]**

| Offset | Field |
|---|---|
| 0 | packet type: 0 query, 1 query-ACK, 2 reply, 3 reply-ACK |
| 2 | query type: 0 generic, 1 UVC/UAC, 2 time |
| 4 | payload size (= datagram − 8) |
| 6 | sequence id (same across one transaction) |

Exchange: console → query(0); pad → ACK(1); pad → reply(2); console → ACK(3).
Retry until ACKed. A real GamePad retries 5× at 33 ms; Barista times out at
1 s with up to 10 attempts. **We must ACK every reply**, or the pad keeps
resending.

### 6.2 Generic command header (12 bytes, after the transport header) **[C]**

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | magic `0x7E` |
| 1 | 1 | version (`0x01`) |
| 2 | 3 | BE 12-bit transaction id + 12-bit fragment id |
| 5 | 1 | flags: `0x40` for a query |
| 6 | 1 | service id |
| 7 | 1 | method id |
| 8 | 2 | BE error code |
| 10 | 2 | BE payload size |

Replies set flags to `((qflags >> 3) & 0xFC) | 1`. **[C]**

### 6.3 Commands padlink sends

| What | Bytes after the 8-byte header | Reply |
|---|---|---|
| **UIC config read** (service 5, method 6) | `7e 01 00 08 00 40 05 06 00 00 00 00` | 12 + 772 bytes: 4-byte UIC firmware version + 768 bytes of UIC config (holds the touch calibration). We keep it **in memory only** and never write it to disk or logs. **[O]** |
| **UVC/UAC** (query type 1), sent about once a second as a keepalive | 48 bytes, zeroed, LE u16 `16000` at offset 12 (mic frequency). After the first reply: byte 0 = 1, and the mic volume/enable fields are copied from the reply. | 16 bytes **[O]** |
| LCD brightness (service 5, method 0x14) | `7e 01 00 08 00 40 05 14 00 00 00 01 <1..5>` | 12 bytes |
| Power off (service 4, method 0x1a, flags `0x42`) | – | **[O]** |

Barista considers the session ready once **input packets are arriving and a
UVC/UAC reply has been received**.

---

## 7. Media timestamps and TSF drift ⚠

**TSF** (Timing Synchronization Function) is a 64-bit microsecond counter
inside the Wi-Fi chip. The AP sends it in every beacon, and every station
(here, the GamePad) keeps its own clock locked to it.

**The rule [C]:** VSTRM and ASTRM timestamps are the **low 32 bits of the AP's
TSF**, in µs. The GamePad schedules decoding and display against its copy of
that clock.

**Why older projects froze [C]:** if the timestamps come from a different
clock (the PC's system clock), the two clocks drift apart by a few ppm. After
a while every frame looks "late" (or far in the future) to the GamePad, so it
stops showing frames and keeps asking for IDRs. The fixes so far:

| Project | Approach |
|---|---|
| libdrc (2013) | Kernel patch exporting `/sys/class/net/<if>/tsf`. Worked on rt2800usb; ath9k still drifted. |
| Barista (2026), **on RTL8852BE** | Reads the **AP port-0 TSF hardware registers** (`0xC438` low, `0xC43C` high, read high-low-high to catch rollover) through rtw89 **debugfs** `/sys/kernel/debug/ieee80211/<phy>/rtw89/read_reg`. Samples every 100 ms and fills in between with `CLOCK_MONOTONIC`. Rejects a sample if the read takes more than 5 ms or jumps by more than ±20 ms. **Stops sending media rather than fall back** to a wrong clock. Needs root and debugfs. |
| Barista fallback | Reads the radiotap TSFT of frames captured on a monitor vif, using kernel RX timestamps (`SO_TIMESTAMPNS`) to remove queueing delay. |
| vanilla | Not applicable (it is the GamePad side). |

**padlink:** a `MediaClock` interface with backends:
1. rtw89 debugfs AP-TSF registers (Barista's method; works on the RTL8852BE),
2. mac80211 debugfs `netdev:<if>/tsf`, if rtw89 implements `get_tsf` for AP
   mode **[?]**,
3. monitor-vif radiotap TSFT.

Media is **paused** while the clock is unhealthy. Every backend is
read-only; none writes to hardware registers.

---

## 8. MSG (pad → console 50010)

- `01 00 00 00` (4 bytes): **"please send an IDR"**. The pad sends it after a
  corrupted or missing frame. It can repeat, so the count of requests is not
  the count of broken frames. **[C]**
- Other contents: unknown **[?]**.

---

## 9. Session startup, end to end

```
GamePad power on
  └─ 802.11 assoc + WPA2 (Nintendo selectors, PTK rot3)
  └─ DHCP → 192.168.1.11
  └─ starts sending HID @180 Hz to :50022
console:
  ├─ CMD: UIC config read (once), UVC/UAC keepalive (~1 Hz), ACK all replies
  ├─ wait for media clock healthy
  ├─ every 16.68 ms: ASTRM format msg → ~5 ms → VSTRM frame (first = IDR + init)
  ├─ every 8.67 ms: ASTRM PCM
  └─ on MSG 01000000: next frame is IDR
no packets for 3 s → treat as disconnected and wait for reassociation
```

**Observed on hardware (2026-10-13):** with input + CMD keepalive working but
**no video**, the GamePad disconnects by itself after about 46 s. It
reassociates once about 9 s later and leaves again within 50 ms. Most likely
cause: it gives up waiting for a video stream (byte 83 bit 0 stayed "waiting
for video"). Confirmed: with video it stays connected. Resting stick values on one pad:
2034 / 2180 / 2106 / 1913 (not 2050), so padlink learns the centers when the
GamePad connects.

## 10. Open questions to settle on hardware

| # | Question |
|---|---|
| Q1 | ✅ Resolved 2026-10-13: the rtw89 debugfs AP TSF works. It resets when the AP starts, stays healthy, and drifts only ±3–11 ppm against CLOCK_MONOTONIC. |
| Q2 | Does the GamePad accept our own (random, locally administered) AP MAC, or does it need a specific OUI? |
| Q3 | Accelerometer and gyro axis order and sign |
| Q4 | ✅ Stick Y direction: raw Y grows upward. Resting centers vary per pad, so padlink learns them on connect. |
| Q5 | ✅ Chunk boundaries: cut where our patched x264 reports the macroblock rows end. |
| Q6 | Meaning of format bytes 4–7 and the 21000 fields |
