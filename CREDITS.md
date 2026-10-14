# Credits and references

padlink is an independent implementation. The projects below were studied
to learn the Wii U GamePad (DRC) protocol. **No code from them has been
copied or adapted into padlink**. If that ever changes, the file,
the origin and the license will be listed under "Adapted code".

## Projects studied

| Project | License | What we learned from it |
|---|---|---|
| [BetaZay/Barista](https://github.com/BetaZay/Barista) | GPL-2.0 | Console-side (DRH) implementation developed on the RTL8852BE: pairing SSID/PIN/credential details, hostapd changes needed for rtw89, AP-TSF register clock, media timing measured against real console captures, VSTRM/ASTRM layouts. |
| [vanilla-wiiu/vanilla](https://github.com/vanilla-wiiu/vanilla) and its wiki ("Pipe", "Wireless Compatibility") | GPL-2.0 | GamePad-side implementation: ports, input packet, CMD replies, MSG IDR request, PTK rotation, hardware compatibility list. |
| [rolandoislas/libdrc](https://github.com/rolandoislas/libdrc) (mirror of memahaxx libdrc) | BSD-2-Clause | Original protocol documentation (Wi-Fi obfuscation, services, VSTRM/ASTRM/CMD/HID formats), touch calibration, TSF requirement. |
| [rolandoislas/drc-sim](https://github.com/rolandoislas/drc-sim) | GPL-2.0-or-later | Older Python GamePad emulator; historical context for the wpa_supplicant-based flow. |

Indirect references: hostapd/wpa_supplicant by Jouni Malinen and contributors
(BSD-3-Clause), which our patches target; minih264 by lieff (CC0-1.0), the
base of Barista's encoder.

## Adapted code

None. The hostapd patches in `third_party/hostapd/patches` were written from
scratch against upstream hostapd 2.11. Barista's patch descriptions told us
*which* behaviors the GamePad and rtw89 need. Our implementation also differs:
the Nintendo selectors only touch the RSN IE writer and parser, so no nl80211
translation is needed.

Observed protocol constants (not code) taken from Barista's real-console
capture notes: the pairing beacon's Broadcom vendor IEs, the WPS registrar
identity strings, the GamePad's DHCP options, and the RTL8852BE AP TSF
register addresses (0xC438/0xC43C).

Bundled third-party code: `third_party/doctest/doctest.h` (doctest 2.4.11, MIT).

Linked library: **x264** (GPL-2.0-or-later), the H.264 encoder. It is built
from upstream source by `third_party/x264/build.sh`, with one small patch of
our own that reports where each macroblock row ends in the CABAC stream (the
same idea as libdrc's "DRH mode" x264 fork, written independently). The GamePad's fixed SPS/PPS bytes and slice header words in
`src/video/h264.h` are protocol constants documented by libdrc (BSD-2-Clause)
and vanilla.

## Not included, by design

No Nintendo firmware, keys, certificates, EEPROM dumps or other copyrighted
assets are in this repository. The GamePad's UIC config, which padlink reads
at runtime for touch calibration, is kept in memory only.
