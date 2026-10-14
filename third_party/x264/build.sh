#!/usr/bin/env bash
# Builds x264 (GPL-2.0-or-later) with padlink's row-offset patch as a static
# library. Nothing is installed system-wide.
# usage: build.sh <work-dir> <install-prefix>
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
work="$1"
prefix="$2"
repo="https://code.videolan.org/videolan/x264.git"
commit="0480cb05fa188d37ae87e8f4fd8f1aea3711f7ee"   # 2025-09-10, X264_BUILD 165

src="$work/x264"
stamp="$work/.padlink-patchset"
patch_hash="$(cat "$here"/patches/*.patch | sha256sum | cut -d' ' -f1)"

if [[ ! -d "$src/.git" ]]; then
    rm -rf "$src"
    git clone --quiet "$repo" "$src"
fi
if [[ "$(git -C "$src" rev-parse HEAD^ 2>/dev/null)" != "$commit" || ! -f "$stamp" || "$(cat "$stamp")" != "$patch_hash" ]]; then
    git -C "$src" am --abort >/dev/null 2>&1 || true
    git -C "$src" fetch --quiet origin "$commit" 2>/dev/null || true
    git -C "$src" reset --quiet --hard "$commit"
    git -C "$src" clean --quiet -xfd
    git -C "$src" -c user.name=padlink -c user.email=padlink@localhost.invalid \
        am --quiet "$here"/patches/*.patch
    echo "$patch_hash" > "$stamp"
fi

asm=()
if ! command -v nasm >/dev/null; then
    echo "x264: nasm not found, building without assembly (slower; install nasm for full speed)" >&2
    asm=(--disable-asm)
fi
cd "$src"
./configure --prefix="$prefix" --enable-static --enable-pic --disable-cli --disable-opencl \
    --bit-depth=8 --chroma-format=420 "${asm[@]}" >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null
echo "built $prefix/lib/libx264.a"
