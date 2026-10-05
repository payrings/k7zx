# Verification tools

None of these tools is built by CMake. All are for checking the converter, not for
using it.

## `k7zx43/`: the original engine, as an oracle

`build.sh [DIR]` builds k7zx 4.3's own conversion engine (`original/zxwav.cpp`,
`ZXCODE.CPP`, `rutinas.cpp`, `zxfiles.cpp`) with g++ into `DIR/k7zx43`
(default `build/k7zx43`). `fixup.py` makes only mechanical edits: an explicit
`int` on implicit-int functions, the stray space in `<stdlib.h >`, and the two
VCL file-name helpers. `drv.cpp` sets the engine's globals the way 4.3's GUI
did and calls `convierteHI()`:

    build/k7zx43/k7zx43 in.tap out.wav METHOD SPB RATE SCHEME [WAVE] [CHECKSUM]

`make_golden.py K7ZX43 tests/data/golden.tap tests/golden_k7zx43.inc`
regenerates the table the unit tests compare the port against. It is only
needed if `golden.tap` or the set of offered speeds changes. It lists the two
deliberate divergences it skips.

## `zxload/`: a Spectrum to play the WAV into

`fetch.sh [DIR]` fetches floooh/chips' cycle-stepped Z80 (zlib licence) and the
48K/128K ROMs (from floooh/chips-test) at pinned commits, then builds
`DIR/zxload` (default `build/zxload`).

`fetch-local.sh [DIR]` builds the same thing **without the network**. It takes
the three Amstrad ROMs from a local emulator install (it finds Fuse's
`48.rom` / `128-0.rom` / `128-1.rom`, which are the same images -- 48K md5
`4c42a2f075212361c3117015b107ff68`) and needs only one file that cannot be
derived locally: chips' `z80.h`. If you have it, drop it at

    build/zxload/third_party/chips/chips/z80.h

and the script builds `build/zxload/zxload`.

    zxload game.wav out.mem [--128] [--contention] [--speed 1.01]
                                   [--extra-seconds N]
    python3 tools/zxload/check.py game.tap out.mem   # OK, or what differs

`zxload` types `LOAD ""` (48K) or chooses the Tape Loader (128K), plays the
WAV with T-state-accurate edges, and stops when the CPU halts with interrupts
off. The test tapes end in `DI; HALT`; so does `tests/data/golden.tap`.
`--dump-at ADDR FILE`, `--watch ADDR [N]` and `--dump128 FILE` help when
something does not load. `gen_sna.py` and `gen_sna128.py` make runnable
snapshots, checked with `checksna.py` and `check128.py`. `wavcmp.py` compares
two WAVs as run lengths, and `stdblocks.py` decodes and diffs their
standard-speed loader blocks.

`sweep.sh build/k7zx-cli build/zxload/zxload` runs every technique × speed ×
rate × scheme through it (about three minutes).

`zxload` also accepts an edge file (`.edge`, written by `channel/channel.py`)
in place of a WAV, and has `--preload FILE ADDR`, `--jp ADDR`, `--tape-at T`
and `--itrace` for testing loader code directly without going through BASIC.
In `--128` mode the CPU clock is 3.5469 MHz and the edges are scaled to match.

## `rayo/`: the Rayo loader's assembler step

    python3 tools/rayo/build_loader.py [path/to/sjasmplus]

This assembles `src/core/asm/rayo.asm` (sjasmplus 1.20 or later) and rewrites
`src/core/rayo_loader.inc`: the loader image, the boot code and the addresses
the converter patches. The `.inc` file is committed, so this is only needed
after editing the assembly.

## `channel/`: a model of a real playback chain

`channel.py IN.wav OUT.edge [--fc HZ] [--fhp HZ] [--offset F] [--noise F]
[--speed F] [--hyst F]` turns a WAV into the edges a real EAR input would see.
The model covers a DAC with band-limited interpolation, a 4th-order output
low-pass, AC coupling, tape speed error, noise, and a comparator with threshold
offset and hysteresis. Feed the result to `zxload`.

    python3 tools/channel/robustness.py build/k7zx-cli build/zxload/zxload \
        tests/data/golden.tap [technique:speed ...]

This runs each technique through ten channel conditions, on 48K timing with
contention and on 128K timing. It prints the wrong-byte counts (README, step
14). It needs numpy and scipy.

### check_findings.py

Re-derives every finding the guide and the README publish from the source and
fails if any has drifted: the per-speed ratings (against the measurement snapshot
`channel/ratings.json`), the bit rates, the technique and test counts, the
golden-table and sweep sizes, and that every documented option still exists.
Run by `ctest` as `k7zx-findings`; can be run by hand:

    tools/check_findings.py .

Re-run `channel/robustness.py` and regenerate `channel/ratings.json` whenever the
signal path or the condition list changes, then confirm the guide still agrees.
