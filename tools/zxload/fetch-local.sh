#!/bin/sh
# Build zxload without network access.
#
#   tools/zxload/fetch-local.sh [BUILD_DIR]     (default build/zxload)
#
# fetch.sh clones floooh/chips and floooh/chips-test. This script instead takes
# the three Amstrad ROMs from a local Fuse/other install, so the only thing it
# cannot do by itself is obtain chips' `z80.h`. Drop that one file at
#
#     build/zxload/third_party/chips/chips/z80.h
#
# (floooh/chips, commit 9e88298ce56319953ac7a43213a1120359f7a3a6, zlib licence)
# and run this again. Everything else -- the ROMs, the build, the layout zxload
# expects -- is handled here.
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=${1:-"$repo/build/zxload"}
mkdir -p "$out/roms" "$out/third_party/chips/chips"

# --- the Amstrad ROMs, from a local emulator's copy if one is installed -------
# zxload wants these three names; Fuse and friends use their own. Fuse's are
# the same Amstrad images (48K md5 4c42a2f075212361c3117015b107ff68).
found=""
for d in /usr/share/fuse /usr/lib/fuse /usr/local/share/fuse /usr/share/spectrum-emu \
         /usr/share/hatari /usr/share/zsnes; do
    [ -f "$d/48.rom" ] && [ -f "$d/128-0.rom" ] && [ -f "$d/128-1.rom" ] && { found=$d; break; }
done
if [ -n "$found" ]; then
    cp "$found/48.rom"    "$out/roms/amstrad_zx48k.bin"
    cp "$found/128-0.rom" "$out/roms/amstrad_zx128k_0.bin"
    cp "$found/128-1.rom" "$out/roms/amstrad_zx128k_1.bin"
    echo "ROMs: $found -> $out/roms"
else
    echo "No local 48.rom/128-0.rom/128-1.rom found. Copy them yourself:" >&2
    echo "  $out/roms/amstrad_zx48k.bin       (16384 bytes)" >&2
    echo "  $out/roms/amstrad_zx128k_0.bin    (16384 bytes)" >&2
    echo "  $out/roms/amstrad_zx128k_1.bin    (16384 bytes)" >&2
    exit 1
fi

# --- chips' z80.h ------------------------------------------------------------
if [ ! -f "$out/third_party/chips/chips/z80.h" ]; then
    cat >&2 <<EOF

Missing: $out/third_party/chips/chips/z80.h

This is the one thing that needs the network. Either:

  1. run tools/zxload/fetch.sh (clones floooh/chips and floooh/chips-test), or
  2. fetch just that file and drop it in place:

       https://raw.githubusercontent.com/floooh/chips/9e88298ce56319953ac7a43213a1120359f7a3a6/chips/z80.h

     It must define z80_t, z80_init, z80_prefetch, z80_opdone, z80_tick and
     Z80_M1, which is all zxload.c uses.
EOF
    exit 1
fi

cc -O2 -w -I"$out/third_party/chips/chips" -o "$out/zxload" "$here/zxload.c" -lm
echo "$out/zxload"
