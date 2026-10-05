#!/bin/sh
# sweep.sh K7ZX_CLI ZXLOAD [TAPE] -- convert TAPE with every technique, speed,
# sample rate and scheme, load each WAV in zxload, and report PASS/FAIL per
# case (PASS = the program ran to its DI;HALT and every CODE block is intact).
# TAPE defaults to tests/data/golden.tap, whose CODE block ends in DI;HALT.
cli=$1; zx=$2
here=$(cd "$(dirname "$0")" && pwd)
tape=${3:-"$here/../../tests/data/golden.tap"}
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
export XDG_CONFIG_HOME="$tmp/cfg"   # keep k7zx-cli away from the user's settings
run() {  # mode method spb rate scheme [extra option]
    if ! "$cli" -q -m "$1" -t "$2" -s "$3" -r "$4" -c "$5" $6 "$tape" "$tmp/t.wav" 2>"$tmp/err"; then
        grep -q "does not support that speed" "$tmp/err" && return
        echo "$1 $2 $3 $4 $5${6:+ $6} REFUSED $(head -c 70 "$tmp/err")"; return
    fi
    res=$("$zx" "$tmp/t.wav" "$tmp/t.mem" 2>/dev/null)
    chk=$(python3 "$here/check.py" "$tape" "$tmp/t.mem")
    case "$res" in result=HALT*) [ "$chk" = OK ] && v=PASS || v=FAIL ;; *) v=FAIL ;; esac
    echo "$1 $2 $3 $4 $5${6:+ $6} $v"
}
for t in rom milks fsk slow delta raudo ultra npu fi fiq manchester manchester-dif escurrido; do
  for s in 8.00 7.00 6.00 5.00 4.00 3.50 3.00 2.75 2.50 2.25 2.00 1.75 1.50 1.33 1.25; do
    for r in 44100 48000; do for c in one many original; do run hispeed $t $s $r $c; done; done
  done
done
# Rayo (added by this port): one block, with and without LZ compression
for s in 2.75 2.25; do
  for r in 44100 48000; do for x in --compress --no-compress; do run hispeed rayo $s $r one $x; done; done
done
