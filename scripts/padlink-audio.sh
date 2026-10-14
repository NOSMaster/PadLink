#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Sends desktop audio to the GamePad. Run as YOUR user (not root), while
# `sudo build/padlinkd run --audio fifo` is running.
#
#   scripts/padlink-audio.sh            adds a "Wii U GamePad" output device;
#                                       pick it per app or as the default
#   scripts/padlink-audio.sh --default  same, and make it the default output
#   scripts/padlink-audio.sh --mirror   copy your current output to the GamePad
#                                       (you hear it on both)
#
# Ctrl+C removes the device and restores your default output.
set -euo pipefail

fifo=/tmp/padlink-audio
sink=padlink_gamepad
mode="${1:-}"

[[ $EUID -ne 0 ]] || { echo "run this as your normal user, not root" >&2; exit 1; }
command -v pactl >/dev/null && command -v parec >/dev/null || { echo "needs pactl and parec (pipewire-pulse)" >&2; exit 1; }

echo "Waiting for padlinkd to create $fifo ..."
while [[ ! -p $fifo ]]; do sleep 0.5; done

previous_default="$(pactl get-default-sink)"
module=""
cleanup() {
    [[ -n "$module" ]] && pactl unload-module "$module" 2>/dev/null || true
    [[ "$mode" == --default ]] && pactl set-default-sink "$previous_default" 2>/dev/null || true
    echo "GamePad audio stopped."
}
trap cleanup EXIT

if [[ "$mode" == --mirror ]]; then
    source="$previous_default.monitor"
    echo "Mirroring $previous_default to the GamePad."
else
    module="$(pactl load-module module-null-sink sink_name=$sink \
        sink_properties=device.description=Wii-U-GamePad rate=48000 channels=2)"
    source="$sink.monitor"
    if [[ "$mode" == --default ]]; then
        pactl set-default-sink "$sink"
        echo "\"Wii-U-GamePad\" is now your default output."
    else
        echo "Added output device \"Wii-U-GamePad\". Choose it in your sound settings or per app (e.g. pavucontrol)."
    fi
fi

# parec records raw PCM; the GamePad wants 48 kHz stereo signed 16-bit LE.
parec -d "$source" --format=s16le --rate=48000 --channels=2 --latency-msec=10 > "$fifo"
