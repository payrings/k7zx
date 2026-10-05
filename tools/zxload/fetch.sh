#!/bin/sh
# Fetch the Z80 core and the ROMs zxload needs (pinned commits), then build it.
#   tools/zxload/fetch.sh [BUILD_DIR]       (default: build/zxload)
# floooh/chips (z80.h) is zlib-licensed; the ROMs come from floooh/chips-test
# (Amstrad allows the Spectrum ROMs to be distributed for use with emulators).
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=${1:-"$repo/build/zxload"}
CHIPS=9e88298ce56319953ac7a43213a1120359f7a3a6
CHIPS_TEST=dc7176cfc5b6f2fe7db795b82f4e592dd6faae7a
fetch() {  # fetch URL SHA DIR
    if [ ! -d "$3/.git" ]; then
        git init -q "$3"
        git -C "$3" fetch -q --depth 1 "$1" "$2"
        git -C "$3" checkout -q FETCH_HEAD
    fi
}
mkdir -p "$out/roms" "$out/third_party"
fetch https://github.com/floooh/chips.git "$CHIPS" "$out/third_party/chips"
fetch https://github.com/floooh/chips-test.git "$CHIPS_TEST" "$out/third_party/chips-test"
for r in amstrad_zx48k.bin amstrad_zx128k_0.bin amstrad_zx128k_1.bin; do
    cp "$out/third_party/chips-test/examples/roms/$r" "$out/roms/"
done
cc -O2 -w -I"$out/third_party/chips/chips" -o "$out/zxload" "$here/zxload.c" -lm
echo "$out/zxload"
