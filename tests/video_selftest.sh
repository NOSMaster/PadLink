#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Encodes the test pattern GamePad-style, rebuilds a standard stream, decodes
# it strictly with FFmpeg and compares it with the input (PSNR).
set -euo pipefail
selftest="$1"; ffmpeg="$2"; split="${3:-rows}"
dir="$(mktemp -d)"; trap 'rm -rf "$dir"' EXIT
"$selftest" "$dir/out.h264" "$dir/in.yuv" 120 "$split"
# -apply_cropping 0: compare all 864 coded columns (the GamePad crops to 854).
"$ffmpeg" -v error -xerror -apply_cropping 0 -f h264 -i "$dir/out.h264" -f rawvideo -pix_fmt yuv420p "$dir/dec.yuv"
in=$(stat -c %s "$dir/in.yuv"); dec=$(stat -c %s "$dir/dec.yuv")
[[ "$in" == "$dec" ]] || { echo "decoded size $dec != input size $in"; exit 1; }
psnr=$("$ffmpeg" -v info -f rawvideo -pix_fmt yuv420p -s 864x480 -i "$dir/dec.yuv" \
                 -f rawvideo -pix_fmt yuv420p -s 864x480 -i "$dir/in.yuv" \
                 -lavfi psnr -f null - 2>&1 | sed -n 's/.*average:\([0-9.inf]*\).*/\1/p')
echo "decoded OK, average PSNR ${psnr} dB"
[[ "$psnr" == inf ]] || awk -v p="$psnr" 'BEGIN { exit !(p > 30) }' || { echo "PSNR too low"; exit 1; }
