#!/usr/bin/env bash
# Builds upstream hostapd 2.11 with padlink's GamePad patches.
# usage: build.sh <work-dir> <output-dir>
# Nothing is installed system-wide; binaries land in <output-dir>.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
work="$1"
out="$2"
repo="https://w1.fi/hostap.git"
tag="hostap_2_11"
commit="d945ddd368085f255e68328f2d3b020ceea359af"   # peeled hostap_2_11

src="$work/hostap"
stamp="$work/.padlink-patchset"
patch_hash="$(cat "$here/hostapd.config" "$here"/patches/*.patch | sha256sum | cut -d' ' -f1)"

if [[ ! -d "$src/.git" ]]; then
    rm -rf "$src"
    git clone --quiet --depth 1 --branch "$tag" "$repo" "$src"
fi
if [[ "$(git -C "$src" rev-parse HEAD^{commit} 2>/dev/null)" != "$commit" || ! -f "$stamp" || "$(cat "$stamp")" != "$patch_hash" ]]; then
    git -C "$src" am --abort >/dev/null 2>&1 || true
    git -C "$src" reset --quiet --hard "$commit"
    git -C "$src" clean --quiet -xfd
    git -C "$src" -c user.name=padlink -c user.email=padlink@localhost.invalid \
        am --quiet "$here"/patches/*.patch
    echo "$patch_hash" > "$stamp"
fi

cp "$here/hostapd.config" "$src/hostapd/.config"
make -C "$src/hostapd" -j"$(nproc)" hostapd hostapd_cli >/dev/null
mkdir -p "$out"
cp "$src/hostapd/hostapd" "$out/padlink-hostapd"
cp "$src/hostapd/hostapd_cli" "$out/padlink-hostapd_cli"
echo "built $out/padlink-hostapd"
