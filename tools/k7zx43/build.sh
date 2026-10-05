#!/bin/sh
# Build the unmodified k7zx 4.3 conversion engine from original/ as a Linux
# command-line tool, k7zx43.  Usage: tools/k7zx43/build.sh [BUILD_DIR]
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
out=${1:-"$repo/build/k7zx43"}
mkdir -p "$out"
python3 "$here/fixup.py" "$repo/original" "$out/src"
cp "$here/shim_c.h" "$here/drv.cpp" "$out/src/"
cd "$out/src"
for f in zxwav.cpp zxcode_impl.cpp rutinas.cpp zxfiles.cpp drv.cpp; do
    g++ -std=gnu++17 -fpermissive -w -O1 -I. -c "$f" -o "${f%.cpp}.o"
done
g++ -o "$out/k7zx43" zxwav.o zxcode_impl.o rutinas.o zxfiles.o drv.o
echo "$out/k7zx43"
