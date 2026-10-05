# k7zx 5.0

A ZX Spectrum tape-file-to-audio converter for Linux.

`k7zx` turns a tape image (`.tap`, `.tzx`, `.sna`, `.z80`, `.sbb`, `.hex`) into
a WAV file that a real Spectrum — or any standards-following emulator — will load.

It is a rewrite of **Francisco Villa**'s k7zx 4.3's conversion engine, and it is faithful to it:
**345 of the 354 conversions 4.3 offered produce a WAV byte-identical to the one
4.3 itself writes**, checked mechanically against 4.3's own engine built from
source. The nine exceptions are two deliberate divergences, described under
[Fidelity](#fidelity-to-k7zx-43). On top of that it adds a fourteenth loading
technique, **Rayo**, which does not exist in 4.3.

```
k7zx                     graphical interface (GTK 3)
k7zx-cli                 headless converter
```

The conversion engine is free of GUI dependencies and has no dependency beyond a
C++20 compiler. `GUIDE.md` is the user guide: how to drive the program, how to
pick a technique, and per-speed reliability ratings.

---

## Contents

- [What it does](#what-it-does)
- [Building](#building)
- [Using the command line](#using-the-command-line)
- [Using the GUI](#using-the-gui)
- [Choosing a technique](#choosing-a-technique)
- [How the output works](#how-the-output-works)
- [The Rayo loader](#the-rayo-loader)
- [Reliability](#reliability)
- [Fidelity to k7zx 4.3](#fidelity-to-k7zx-43)
- [Known limits](#known-limits)
- [Configuration](#configuration)
- [Repository layout](#repository-layout)
- [Verification](#verification)
- [Development notes](#development-notes)
- [Credits](#credits)
- [Licence](#licence)

---

## What it does

k7zx reads a tape image and writes a WAV. There are two conversion modes.

| Mode | What it produces |
|---|---|
| **Normal** | A verbatim replay of the tape: 2168 T-state pilot pulses, a 667/735 sync pair, 855/1710 T-state data bits. This is what `tap2wav` and `tzx2wav` do. Use it for archiving, or for a strict emulator that will not tolerate a turbo loader. |
| **High speed** | A synthetic BASIC program carrying a hand-assembled Z80 routine that reads the tape with a fast encoding, plus the payload re-serialised. Loads in seconds rather than minutes. |

### Input formats

`.tap`, `.tzx`, `.sna`, `.z80`, `.sbb` and `.hex` — tape images in the standard
formats, plus 48K and 128K snapshots (`.sna`, `.z80`) and the SCL86 `.sbb`
container.

The `.tzx` reader handles the 25 block types k7zx 4.3 handled and accepts both
the 10-byte and the 12-byte TZX header, so files from other tools load as well
as the ones k7zx writes.

### Loading techniques

Thirteen techniques come from k7zx 4.3, from the Spectrum's own ROM loader up to
a 38,400 bps routine:

```
ROM    Milks    FSK    Shavings Slow   Shavings Delta   Shavings Raudo
Ultra   NPU     Fi     Fi Quadruple    Manchester        Man. diferencial
Escurrido
```

This port adds a fourteenth:

* **Rayo** — designed for this port, and the only one of the fourteen that
  compresses the data. Runs at 2.25 and 2.75 samples per bit, adds an
  end-of-tape check so a corrupt load stops with an error instead of running,
  auto-detects polarity, and tolerates a key held during loading. Compression
  makes a typical game's whole load about 30 % shorter. **See
  [The Rayo loader](#the-rayo-loader)**.

**For MP3 output, streaming or a cassette recording, use FSK at 5.00–7.00
samples per bit** (`-t fsk -s 5.00`). Those are the FSK speeds measured clean
through every analogue channel condition on both 48K and 128K timing.

k7zx 4.3 also had a low-rate "Ma non troppo" mode aimed at the same use case.
Measurements showed FSK at 5.00–7.00 survives lossy encoding and a real tape
equally well while being considerably faster to load, so the mode was removed.
The `Veloz` and `Ma non troppo` technique names are likewise gone; a saved
configuration naming one is migrated to FSK at 5.00 rather than silently
switching to something else.

---

## Building

Requirements: a C++20 compiler, CMake ≥ 3.16, and — for the GUI only —
`gtkmm-3.0`.

```sh
# everything
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build          # optional

# core + CLI only (no GTK needed)
cmake -S . -B build -DK7ZX_BUILD_GUI=OFF
cmake --build build -j
```

Debian/Ubuntu dependencies:

```sh
sudo apt install cmake g++ libgtkmm-3.0-dev
```

Run the tests:

```sh
cd build && ctest --output-on-failure
```

---

## Using the command line

```sh
k7zx-cli [options] <input.tap|tzx|sna|z80|sbb|hex> [output]
```

```sh
# a 17 kbps "Shavings Raudo" load of a tape file
k7zx-cli -t raudo -s 2.75 game.tap out.wav

# see what was found in the file first
k7zx-cli -v game.tap

# a verbatim replay, for archiving or for a strict emulator
k7zx-cli -m normal game.tap out.wav

# only convert two of the seven blocks
k7zx-cli --blocks 0,2 game.tap out.wav

# an mp3 as well as the WAV
k7zx-cli --mp3 -t fsk -s 5.00 game.tap out.mp3

# a TZX direct recording, for an emulator
k7zx-cli --emulate -t fiq -s 2.5 game.tap out.tzx

# Rayo: compressed by default, --no-compress turns compression off
k7zx-cli -t rayo -s 2.75 game.tap out.wav
```

The full option list is in [GUIDE.md §8](GUIDE.md#8-command-line) and in
`k7zx-cli --help`.

Settings are read from `$XDG_CONFIG_HOME/k7zx/k7zx.ini` (see
[Configuration](#configuration)). An option you type overrides the stored value;
an option you leave out does not. `k7zx-cli` never writes the file — only the
graphical program saves changes.

---

## Using the GUI

`k7zx` opens on two tabs.

**Settings** has the browser along the top — a directory tree, a path box, a file
list and a file-info pane — then the encoding options, then the transport
buttons and output settings. **About** has the advanced overrides (locate,
relocate, pokes) and the per-technique help text.

The window opens at the size its content needs and is laid out so no group
needs a scroll bar. The choices that matter are **Loading method** (the
technique and its speed) and **Loading scheme**; everything else has a sensible
default. GUIDE.md walks through each control.

```sh
k7zx                                          # open on the home directory
k7zx game.tap                                 # open with a file loaded
k7zx -p -t fsk -s 5.00 game.tap               # convert and play, no window
k7zx --self-test game.tap                     # drive every control and report
```

---

## Choosing a technique

Every technique was tested at every speed it offers, in an emulator and through
a model of a real playback chain (DAC, low-pass filter, AC coupling, tape speed
error, noise, and a comparator with threshold offset and hysteresis). The
results are per-speed ratings in [GUIDE.md §6](GUIDE.md#6-choosing-a-technique),
with the reasoning, the recommendations, and the measurements behind them.

In short:

- **Just want it to load?** FSK at 5.00.
- **MP3, streaming, cassette?** FSK at 5.00–7.00.
- **Fastest reliable speed for a given technique?** See the ratings table.
- **Shavings Raudo, Fi Quadruple, Escurrido and the half-pulse techniques** all
  measure worse through a real analogue chain than the original's own
  yellow/red marks suggested, because they time individual half-cycles and a
  threshold offset shifts their timing directly. With a well-set volume on good
  hardware they behave as k7zx 4.3 claimed.

Of the 61 technique-and-speed combinations measured, **six of the sixty-one are
clean in all ten channel conditions on both machines**: FSK 5.00, FSK 6.00, FSK
7.00, Shavings Raudo 2.25, Ultra 3.00 and Escurrido 2.50.

---

## How the output works

### Normal mode

A faithful transcription of the tape, at the timings the Spectrum's ROM expects:
a 2168 T-state pilot pulse per block, a 667/735 T sync pair, then 855 T for a
zero bit and 1710 T for a one bit, most significant bit first. Nothing is
invented; what is on the tape is what comes out.

### High speed mode

k7zx writes a tape containing two blocks:

1. A header block and a data block holding a **synthetic BASIC program**. The
   program is a single line — `CLEAR n:RANDOMIZE USR 23781:STR$` — followed by
   the machine code of a Z80 loader routine.
2. The payload, re-serialised with the fast encoding the routine understands.

When the Spectrum loads and runs the BASIC line, the routine takes over the
tape port, measures each pulse, and rebuilds the original bytes in memory.

The loader routines are **data**, not code this project refactors: they are
byte-for-byte copies of the originals (also present as `.asm` listings in
`original/asm/`), patched in place at conversion time to encode the chosen
timing. Every one of them is a *matched pair* — a decoder and a pulse encoder
tuned to each other — which is why a technique is selected as a unit.

Rayo takes a different route. It builds its own loader rather than patching one,
drives the tape through explicit half-cycle timings, and expands the payload in
place so the compressed form can be stored immediately above the decompressor.

---

## The Rayo loader

Rayo is the one addition to k7zx 4.3's thirteen techniques, and it is the only
one that **compresses the data**. It runs at two speeds — 2.25 (21,333 bps) and
2.75 (17,454 bps) at 48 kHz — and shares Raudo's signal family: two bits per
wave cycle, with the cycle length chosen from four possibilities.

### What compression does and does not do

It is worth being precise, because the effect is easy to overstate. Compression
does **not** raise the bit rate. Rayo still sends 2 bits per cycle at the same
speeds. What it changes is *how much* there is to send: a typical 48K game's
compressed form is about two-thirds the size, so the tape carries fewer bits and
the load finishes sooner. It is a smaller download, not a faster modem.

That still matters. A game that does not fit comfortably at 21,333 bps
uncompressed can finish in about the time NPU 1.25 takes uncompressed — but only
if the game compresses well, and the loader shows no loading screen while it
happens (see below).

### The compressed format

An LZ77 variant with interlaced Elias gamma codes, written MSB first. The bit
buffer is pulled from the byte stream as each bit is needed, so bits and bytes
interleave freely.

```
stream   := literal { token } end
literal  := gamma(n) <n bytes>                          n >= 1
after a literal:  0 -> repmatch,  1 -> newmatch
after a match:    0 -> literal,   1 -> newmatch
repmatch := gamma(n)                copy n bytes from the previous offset
newmatch := gamma(hi+1) <lo> gamma(n-1)   offset = hi*256 + lo + 1, n >= 2
end      := 1 gamma(256)
```

Two things keep it compact. A **repmatch** repeats the offset already in use —
which is what happens constantly in a program's memory, where the same jump
table or the same data pattern recurs a few hundred bytes apart — and costs
nothing but a length code. A **newmatch** costs a length, a high byte and a low
byte, giving a 16-bit offset.

The compressor is not greedy. It builds, for every position, the cheapest
parse by dynamic programming over the cost of ending on a literal run versus
ending on a match, then backtracks the cheapest path.

### Expanding in place, with no spare memory

This is the part that makes it work on a 48K Spectrum at all.

A decompressor cannot overwrite the compressed data it is still reading. The
usual answers are a second buffer — which a Spectrum does not have to spare —
or a two-pass scheme that costs time. Rayo uses neither.

The compressed stream is placed **immediately above** the region it will expand
into, and the write pointer trails the read pointer:

```
   uncompressed:   [ dst ...................... dst+N ]
   compressed:                            [ dst+N+d-C ... dst+N+d )
```

The decompressor runs with `HL` (read) at the start of the compressed data and
`DE` (write) at `dst`. Each output byte advances `DE` by one; the input advances
by zero or more. `DE` chases `HL` but never catches it, so it never overwrites
input it has not read — provided the gap `d` was large enough. The whole
decompressor is **76 bytes** of Z80 (`$FD54`–`$FDA0`), and it runs once, after
the tape has finished.

`d` is not guessed. Before writing the tape the converter runs the reference
decompressor on the machine, recording for every output byte how many input
bytes had been consumed by then, and takes the largest difference. It also
decompresses the result and compares it byte for byte against the original.

If any of three things is wrong, the tape is written **uncompressed** and the
conversion log says which one it was:

- the round trip did not reproduce the data, so compression is not trustworthy;
- the data does not actually get smaller (random or already-packed data will
  not);
- the compressed stream would not fit between the end of the expanded data and
  the decompressor. That gap is small — the whole payload has to end below
  `$FD40` (`64832`), where the loader itself begins.

Nothing is lost by declining: the load works, it is just not shorter.

### What else Rayo adds

Compression is one of six differences from Raudo:

| | |
|---|---|
| **End-of-tape check and checksum** | A load that would run corrupt stops with a tape error instead. Raudo has neither. |
| **Polarity learnt from the sync** | An inverted signal loads. |
| **Port read as `$FFFE`** | A key held down during loading does no harm. |
| **Per-tape symbol mapping** | The 2-bit values are assigned to cycle lengths by frequency, so the commonest value gets the shortest cycle. Raudo uses a fixed order. |
| **Compression** | Above. |
| **Explicit cycle timings** | Pulses are timed half by half, which a DAC's reconstruction filter reproduces more faithfully. |

### The one real cost

**With compression on, the loading screen does not appear while the data
loads.** The compressed data is held high in memory and expanded into place in a
single final pass, so the screen is written all at once at the end rather than
being drawn progressively. Turn compression off (`--no-compress`, or the *Compress
(Rayo)* checkbox) to watch it fill in. For a game with a loading screen that
tells you something, that is the trade.

---

## Reliability

Run through an emulated 48K/128K Spectrum with the real ROMs,
**338 of the 362 cases in the full sweep load.** Every one of the 24 that do not
is byte-identical to k7zx 4.3's own output for the same input, and every one is
a speed k7zx 4.3's own combo box already drew yellow or red:

| | failing cases |
|---|---|
| ROM | 2.50 (both rates, all three schemes) |
| Milks | 1.75, 2.25, 2.50 at 48 kHz (all three schemes) |
| Escurrido | 1.33 at 44.1 kHz; 2.00 at 44.1 kHz in many-block and original-loader schemes |
| Ultra | 1.50 and 2.50 as a single block |

These are the original's limits, reproduced rather than papered over.

Snapshots were checked the same way with runnable 48K and 128K files: every
technique restores all RAM — all eight banks on a 128K — plus every register and
the paging port. The exception is the strip of screen the loader uses to stage
its restore stub.

**Not verified here: nothing has been played into a real Spectrum.** The ratings
come from an emulator plus a model of the analogue chain. A model is not a
cassette deck, and no amount of simulation substitutes for the hardware.

---

## Fidelity to k7zx 4.3

The conversion engine reproduces k7zx 4.3's output. Of the 354 conversions 4.3
offered, **345 of the 354 produce a WAV byte-identical to the one 4.3 itself
writes**,
including every pulse-width table, every loader patch offset, the
1/79.36508 T-states-per-sample factor, the sample amplitudes and the container
headers.

This is checked mechanically rather than by inspection. `tools/k7zx43` builds
4.3's own engine from the pristine sources in `original/`, and the test suite
converts `tests/data/golden.tap` every remaining way and compares the result
byte for byte against a recorded table.

### Two deliberate divergences

- **Fi at 3.00 samples per bit.** The loader differs from 4.3's, and 4.3's does
  not load — run through an emulator its WAV leaves the CPU running, while this
  one's halts cleanly. Fi at 8.00 down to 4.00 is byte-identical, so this is
  confined to the single speed that swaps in the compact `fi_mini` routine.
  These six combinations are excluded from the byte-identity table rather than
  asserted to match. The mechanism behind the difference is not established.
- **Raudo at 1.75, 48 kHz.** The generated BASIC line runs past the end of
  `raudo48000`; 4.3's fixed-length copy filled that padding from whatever
  followed the array in memory, and this port writes zeros. Both load.

### Deliberately inherited quirks

Some original behaviour looks like a bug and is reproduced anyway, because
changing it would break the byte-identity guarantee and the tapes would stop
matching what people already have:

- Standard-speed pulses keep 4.3's timings (a 4336 T period, i.e. two 2168 T
  pilot pulses).
- The "as close to the original loader as possible" scheme `strcat`s the LPRINT
  shim onto the loader body and then takes `strlen` for the declared line length,
  so the shim is appended but excluded from the length.
- The b.p.s. readout follows the sample rate actually written to the file;
  4.3 ignored its divisor and displayed twice the real figure.

---

## Known limits

**Rayo**

- 128K snapshots are refused, with a message pointing at Shavings Raudo. 48K
  snapshots, and tapes loaded as one block, work.
- One block, which must end below `$FD40`. Anything higher is clipped, with a
  warning.
- The XOR checksum is not airtight: two wrong bytes in the same bit position
  cancel. A CRC would close the gap.
- No loading screen while compressing. The compressed data loads high in memory
  and is expanded into place, screen included, only at the end.

**Both front ends**

- The batch dialog writes to the final path rather than via a temporary file, so
  an interrupted batch leaves a truncated file that looks complete.
- mp3 encoding shells out to `lame` (or `ffmpeg`, or `$K7ZX_MP3_ENCODER`); there
  is no built-in encoder. The bit rate is fixed at 320 kbps -- the highest
  MPEG-1 Layer III allows, and the only rate at which every technique in the
  guide's lossy-codec table comes out of the encoder clean.

---

## Configuration

Both programs read `$XDG_CONFIG_HOME/k7zx/k7zx.ini`, usually
`~/.config/k7zx/k7zx.ini`. Only `k7zx`, the graphical program, writes it;
`k7zx-cli` reads and never saves. `--config-path` points either at a different
file.

Keys and section names are matched **without regard to case**, as they are on
Windows. Hand-editing the file is therefore safe: `[Ops] Method=8` and
`[ops] method=8` mean the same thing.

```ini
[Ops]
Mode=1                ; 0 = normal, 1 = high speed
Method=5              ; technique, as an integer (see GUIDE.md §11)
SamplesPerBit=11      ; speed, as quarter-units (11 = 2.75)
SampleRate=48000      ; 44100 or 48000
Waveform=2            ; 0 square, 1 ramp, 2 cubic, 3 continuous,
                      ; 4 equal energy, 5 parabola, 6 delta
ManyBlocks=2          ; 1 = one block, 2 = many blocks, 3 = original loader
Final=1               ; append the decaying closing tone
CD=0                  ; 16-bit stereo instead of 8-bit mono
Kolmogorov=0          ; statistical Delta timing
control_chksum=1      ; install the tape-error check where it applies
GenerateLoader=1      ; synthesise the BASIC loader program
compress=1            ; LZ-compress the payload (Rayo only)
```

Settings that are out of range are clamped rather than propagated into the
renderer. A few legacy keys written by earlier versions — including the original
Spanish spellings — are still read, so an existing configuration keeps working;
see [GUIDE.md §11](GUIDE.md#11-reference-tables) for the full table.

---

## Repository layout

```
src/core/        the conversion engine — no GUI dependencies
  defs.h           technique / waveform / speed constants, Spectrum timings
  byteorder.h      bounds-checked little-endian readers and file I/O
  loader.h         Z80 loader routine tables and the patchable working copy
  loader_data.cpp  the tables themselves, transcribed from the original
  texts.h          technique names and the per-technique explanations
  zxfiles.h        tape / snapshot readers
  zxwav.h          the signal generator
  zxcode.h         loader synthesis
  rayo.h           the Rayo loader and encoder
  lz.h             Rayo's LZ compressor
  mp3.h            running an external mp3 encoder
  convert.h        the façade the GUI and CLI share
  settings.h       persisted preferences and the technique/speed tables
  asm/rayo.asm     Rayo's Z80 source; rayo_loader.inc is generated from it
src/cli/         k7zx-cli
src/gui/         the GTK 3 front end
tests/           unit tests, fixture generator, round-trip decoder
tools/           verification tools, none of them built by CMake
GUIDE.md         the user guide, with per-speed reliability ratings
original/        the untouched 1998 sources, for reference
```

`tools/` is described in `tools/README.md`. None of it is needed to build or use
k7zx; it exists to check the converter against the original engine and against
an emulated machine.

---

## Verification

```sh
cd build && ctest --output-on-failure
```

`ctest` runs five things:

- **The fixture generator**, which writes nine synthetic tape images from the
  published format descriptions. No sample data is used.
- **125 unit tests** over the readers, the container writers, the loader tables,
  the speed tables, the LZ codec and the full technique × speed × scheme matrix.
  One converts `tests/data/golden.tap` 345 ways and requires each WAV to be
  byte-identical to k7zx 4.3's own.
- **`tools/check_findings.py`**, which re-derives every number this file and the
  guide publish — the rating rows from the stored measurement, the bit rates,
  the technique and test counts, the golden-table and sweep sizes — and fails if
  any of them has drifted. A measurement is only worth as much as its freshness,
  and a document drifts silently; this makes the drift loud.
- **`tests/roundtrip_tap.py`**, which decodes a generated WAV back into pulse
  lengths and compares the recovered blocks against the source `.tap`.
- **`tests/cli_behaviour.py`**, which checks that the command line does what
  this file and the guide say it does.

Additional checks that are not part of `ctest` because they need extra tools:

- `tools/zxload` loads WAVs into an emulated 48K/128K Spectrum with the real
  ROMs. `tools/zxload/sweep.sh` runs the whole matrix through it.
- `tools/channel` puts a WAV through a model of a real playback chain. The
  per-speed ratings in the guide come from it.
- `tools/k7zx43` builds k7zx 4.3's own engine so the port can be compared
  against it.
- `k7zx --self-test` drives every GUI action with the window realized and the
  event loop pumped, which the unit tests cannot reach.

Current state: clean build under `-Werror`, **125/125 tests**, `ctest` 5/5, and
the suite clean under AddressSanitizer and UndefinedBehaviorSanitizer with leak
detection.

---

## Development notes

**Technique names are part of the interface.** k7zx 4.3 was itself inconsistent
— its options dialog says `S. Slow` while the main window's radio captions say
`Shavings Slow` — and both spellings are reproduced here rather than normalised
away, because that is what saved configurations and documentation refer to.
`tests/test_routines.cpp` pins every name against the original sources, so a
rename or a deletion fails the build rather than quietly changing the product.

**`src/core/rayo_loader.inc` is generated.** It comes from `src/core/asm/rayo.asm`
via `tools/rayo/build_loader.py`, and is committed so that building k7zx does
not need an assembler. Edit the assembly, not the `.inc`.

**`original/` is untouched.** Nothing in it has been edited, and `.gitattributes`
marks the directory so git cannot rewrite its line endings either. It is there to
be diffed against.

---

## Credits

k7zx was written by **Francisco Villa**, who also released the later **OTLA**
tool. The conversion algorithms, the Z80 loader routines and the UI layout are
his work.

Concretely, three things here are his rather than this port's:

- **The Z80 loader routines**, carried byte for byte in
  `src/core/loader_data.cpp` — 36 tables, 4,627 bytes of machine code. They are
  patched in place at conversion time to encode the chosen timing, and nothing
  here ever alters a loader byte except through the patch table the original
  used.
- **`original/`**, the complete 1998 sources, verbatim and unmodified.
- **`original/asm/`**, the same routines as readable `.asm` listings.

Everything else — the portable C++ core, the GTK 3 front end, the command line
tool, the test suite, the verification tools, the documentation, and the Rayo
loader — is this port's. The original source is from the
[otla](https://github.com/sweetlilmre/otla) repository.

The original's own About box adds:

> "Gratefulness to Antonio Villena, Black Hole and good people of
> es.comp.sistemas.sinclair for their inspiration and enthusiasm."

Antonio Villena is the author of
[CargandoLeches](https://github.com/antoniovillena/CargandoLeches) and
[zx7b](https://github.com/antoniovillena/zx7b).

k7zx 5.0 is an independent reimplementation of k7zx 4.3's conversion engine for
modern C++ on Linux, with a new GTK 3 front end, a command line tool, and the
Rayo loader.

## Licence

MIT — see [LICENSE](LICENSE).

This is a rewrite of **Francisco Villa's** k7zx 4.3, and it carries his Z80
loader routines verbatim, as set out under [Credits](#credits).
