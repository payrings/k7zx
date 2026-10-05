# k7zx 5.0 — user guide

k7zx turns a tape image into a **WAV file that a real ZX Spectrum can load**,
or into an mp3 you can keep and re-encode later.

The idea in one paragraph: a Spectrum reads its tape by measuring how long the
level stays high or low. k7zx contains fourteen different schemes for turning
bytes into those long-and-short pulses, each with its own trade-off between
speed and reliability. It builds a tiny machine-code program, sticks it at the
end of a one-line BASIC program, and renders the whole thing as audio.

---

## Contents

1. [Before you start](#1-before-you-start)
2. [Running it](#2-running-it)
3. [The Settings tab](#3-the-settings-tab)
   - [Directories](#directories) · [Files](#files) · [File info](#file-info)
   - [Loading method](#loading-method)
   - [Loading scheme](#loading-scheme) · [Wave properties](#wave-properties)
4. [The buttons](#4-the-buttons)
5. [The About tab](#5-the-about-tab)
6. [Choosing a technique](#6-choosing-a-technique)
   - [How to read the ratings](#how-to-read-the-ratings) · [Recommendations](#recommendations)
   - [MP3, streaming and cassette](#mp3-streaming-and-cassette)
7. [Loading a tape for real](#7-loading-a-tape-for-real)
8. [Command line](#8-command-line)
9. [Troubleshooting](#9-troubleshooting)
10. [The techniques in detail](#10-the-techniques-in-detail)
11. [Reference tables](#11-reference-tables)

---

## 1. Before you start

### What you need

| To | You need |
|---|---|
| Play audio, keep an archive | Nothing — this program |
| Produce mp3 | `lame` (or `ffmpeg` as a fallback) on your `PATH` |
| Load onto real hardware | A cassette deck, or a ZX Spectrum with tape input |
| Load into an emulator | Any emulator that can play a WAV or a TZX |

### Input formats

| Extension | Contents |
|---|---|
| `.tap` | Raw tape blocks. The normal case. |
| `.tzx` | The "extended" container. Same tape data plus turbo, snapshot and metadata blocks. |
| `.sna` | 48K/128K memory snapshot. |
| `.z80` | 48K/128K memory snapshot, versions 1–3. |
| `.sbb` | SCL86 tape block container. |
| `.hex` | A raw hex dump loaded at `0x4000`. |

### The two conversion modes

This is the single most important choice.

#### Normal — verbatim replay

Reproduces the tape exactly as the Spectrum ROM would have written it: a
leader of 2168-T-state pulses, a 667/735 sync pair, then one bit per pulse pair
(855 T-states for a zero, 1710 for a one).

- **Use it to** archive a tape, or to check a loading scheme against a
  reference.
- **Do not use it to** load quickly — it takes as long as the original tape.
- Only `.tap` and `.tzx` make sense here. Snapshots are memory images, not
  tapes, and are rejected with a message.
- The waveform is a smooth cubic curve rather than a square wave, which is
  deliberate: a square wave full of sharp edges is expensive to MP3-encode,
  whereas the cubic one compresses well.

#### High speed (turbo loader) — the default

Synthesises a loader in machine code and re-encodes the payload through one of
the fast schemes. A 190-second game loads in roughly ten seconds.

- **Use it to** get a game onto a real Spectrum or into an emulator quickly.
- A `.tap` or `.tzx` must contain data blocks **with** their headers, and must
  yield a usable `CLEAR` and `USR`. If it does not, fix them in the **About**
  tab. Up to seven blocks can be loaded with *Many blocks*.

#### Producing an mp3 or a cassette recording

There is no separate mode for this. **Use High speed with FSK at 5.00–7.00
samples per bit.** See [MP3, streaming and cassette](#mp3-streaming-and-cassette)
for the test results behind that advice.

From `k7zx-cli` add `--mp3`; the WAV is written first and deleted once the encode
has succeeded, so an mp3 that fails does not cost you the conversion.

---

## 2. Running it

```sh
cd k7zx
./build/k7zx                  # opens on the last folder used
./build/k7zx game.tap          # opens a file directly
```

To install it system-wide:

```sh
sudo cmake --install build     # puts k7zx and k7zx-cli in /usr/local/bin
```

Settings are remembered in `$XDG_CONFIG_HOME/k7zx/k7zx.ini`
(usually `~/.config/k7zx/k7zx.ini`). Only `k7zx`, the graphical program, ever
writes it; `k7zx-cli` reads it and nothing more.

> The first release of the original wrote a few INI keys in Spanish
> (`Cargador`, `Bloques`, `Frecuencia`, …). The port writes English keys but
> still **reads** the old ones, so an existing `k7zx.ini` keeps working.
>
> Keys and section names are matched **without regard to case**, as they are on
> Windows. Hand-editing the file is therefore safe: `[Ops] Method=8` and
> `[ops] method=8` mean the same thing.

**A command-line option only overrides what you actually asked for.** Both
programs read the same file, and an option you did not type leaves the stored
value alone — so a `k7zx.ini` set to 44.1 kHz, stereo and Shavings Slow now
produces the same tape from the GUI and from `k7zx-cli`. (Before this, the
command line carried the documented defaults and copied them over the file
unconditionally, so a flag that was never given still won.)

### The window

The window has two tabs.

- **Settings** — everything you normally need. Browser along the top, the
  encoding options in the middle, the transport buttons and output settings at
  the bottom.
- **About** — credits, manual `CLEAR`/`USR` overrides, and the poke table.

---

## 3. The Settings tab

### Directories

The folder being browsed, shown as an editable path box with a tree of your
home directory's usual subfolders and `/` beneath it. Click a folder to list
its contents; press <kbd>Enter</kbd> in the path box to jump straight to a
typed path.

### Files

Every supported file in the current folder, with size, type and access time.
**Clicking a file loads it** — the blocks and addresses update immediately, and
the Convert / Play buttons become available.

### File info

Everything k7zx worked out about the loaded file:

| Field | Meaning |
|---|---|
| **Program** | The 10-character name from the tape header, or the file name. |
| **CLEAR** | Where the BASIC program expects to be loaded. Everything above this address is safe to overwrite. |
| **RANDOMIZE USR** | The entry point the program will jump to when it starts — the address the loader must finish at. |
| **Blocks** | One row per tape block, with a **Load** tick box to include or exclude it. Type `0` is a BASIC program; types `1`–`3` are number arrays, character arrays and code. |

Ticking and unticking a **Load** box changes what gets converted. Leave every
box ticked unless you know why you would not.

> **Block overrides address $ff3c** — shown in red when a block would extend
> past `0xFF3C` and overwrite the Spectrum's system variables, which will crash
> the machine. Either untick that block, or move the program's `CLEAR` above it
> in the **About** tab.

---

### Loading method

This group chooses *what kind of encoding* is produced: the conversion mode,
the technique, the speed, and a read-out of the resulting bit rate.

#### Conversion

| Entry | Effect |
|---|---|
| `Normal conversion` | Verbatim replay. Forces 44.1 kHz, 8-bit mono. Only `.tap`/`.tzx`. |
| `High speed (turbo loader)` | The default. All fourteen turbo techniques available. |

#### Technique

Fourteen turbo techniques: the thirteen from k7zx 4.3, from the Spectrum's own
ROM loader to the 38,400 bps NPU routine, plus Rayo, which this port adds. The
list only appears in *High speed* mode.

**Each entry has a small (i) button beside it.** Hover for a one-line summary,
click for the full explanation. The text follows your selection, so you can
click through the list and read each one.

The available speeds below each technique are filtered to what that technique
can actually do — you cannot select a combination the loader cannot read.

#### Samples per bit

How long each bit occupies, expressed in samples per bit. **Lower is faster and
less reliable.** The choice is constrained by the technique.

k7zx 4.3 drew each speed in this list green, yellow or red as a reliability
hint. The port's list is plain. The same colours, together with the port's own
test results for every speed, are in
[section 10](#10-the-techniques-in-detail).

The read-out underneath shows the resulting throughput:

```
b.p.s = frequency / (samples per bit)
17454 bps
```

That figure is bits per second of the file that is actually written.

---

### Loading scheme

How the blocks are handed to the machine. Only relevant in *High speed* mode,
and only for tape files — a snapshot is loaded whole, so all three become
unavailable.

| Entry | What it does |
|---|---|
| **All in one block** | Every selected block is joined into one contiguous image and sent as a single stream. The loader sets up the whole memory map, then reads straight through. Simplest and usually the most reliable. |
| **Many blocks** | The loader is given a list of (address, length) pairs and visits each block in turn. Needs a header on every block, so it will not work on a bare `.tap` whose blocks lack one. Up to seven blocks. |
| **As the original loader** | Emulates `LOAD "" CODE`: the program's own `LOAD` statement does the work, and k7zx only supplies the tape. Only available for **Milks**, which is the only technique that writes a self-describing header the ROM can consume. |

#### Generate the BASIC loader

On by default, and you should normally leave it on.

k7zx builds a one-line BASIC program whose payload is a machine-code routine
that reads the tape. Ticking this off stops it writing that program, which is
only useful if you already have a loader on the tape yourself. With it off you
are on your own.

#### Verify tape loading error

Installs an `RST 8` handler so that a checksum failure writes $1A, which the
Spectrum's ROM shows as *"R Tape loading error"*, instead of the machine
crashing.

Greyed out, as in k7zx 4.3, for **ROM**, **Shavings Delta**, **Shavings
Raudo**, **Ultra**, **Fi Quadruple** and **Escurrido**. Do not read that as
"these are checked anyway": Shavings Raudo, for one, has no checksum at all,
and a bad load simply runs corrupt. It is also greyed out for **Rayo**, which
has its own check built in.

#### Statistical optimisation

Only for **Shavings Delta**.

That technique assigns each 2-bit group its own pulse length. Ticking this
counts how often each of the four groups occurs in your data and gives the
commonest group the shortest pulse, which is faster. Leave it off if you are
converting the same data more than once and want identical output.

#### Compress (Rayo)

Only for **Rayo**, and on by default.

The data is LZ-compressed before it goes on the tape and expanded in place
once it has loaded. A typical game has about a third as much to send, which
makes the whole load about 30 % shorter. If the data does not compress, or
there is no room to expand it, it is sent uncompressed and the log says so.
On the command line, use `--compress` / `--no-compress`.

One side effect: **with compression on, the loading screen does not appear
while the data loads.** The compressed data is loaded high in memory and only
expanded into place, screen included, at the very end. Turn compression off if
you want to watch the screen fill in.

---

### Wave properties

The shape of the audio itself.

| Option | Effect |
|---|---|
| **Sampling frequency** | 44100 or 48000 Hz. 48000 is a good default; some older emulators and sound cards cope better at 44100. |
| **Waveform** | The pulse shape. **Cubic** is the safe default. See the note below. |
| **Invert wave** | Flips the polarity of the whole signal. Try it if a tape deck or emulator loads nothing. |
| **Stereo (16 bit)** | Writes a 16-bit stereo file instead of 8-bit mono. Useful with cassette decks that cannot manage a 44.1 kHz mono feed. |
| **Reverse right channel** | Inverts the right channel. Greyed out unless stereo is on. Lets you cancel the signal in a mono mix-down. |
| **Final tone** | Appends a short decaying tone after the data, which gives a deck's automatic-gain circuit something to latch onto. Worth trying if a deck runs out of level. |
| **Accelerated BASIC loader** | Uses the fast loader for the generated BASIC program as well. |

#### A note on waveform

The seven shapes are `Square`, `Ramp`, `Cubic`, `Continuous Compensate`,
`E Compensate`, `Parabola` and `Delta` -- the names the GUI shows. `k7zx-cli`
spells them `square` `ramp` `cubic` `continuous` `energy` `parabola` `delta`.

Several techniques document a preferred shape — Shavings Slow prefers cubic,
Ultra prefers `Continuous Compensate` or `E Compensate`, Manchester prefers
`E Compensate`. **Cubic works with everything**, so only change it if a
technique is not loading and you want to try something else. Rayo always uses
a square wave, whatever is selected here.

---

## 4. The buttons

### Info in output file name

Appends the technique, scheme and speed to the output file name:

```
game_SRA__VBLO_2.75.wav
```

Aimed at keeping a whole collection of different encodes of the same game
apart. Off by default, because the names get long.

### Play

Converts to a temporary WAV and plays it through your sound system. It uses the
first of `paplay`, `pw-play`, `mpv`, `ffplay` or `aplay` that it finds, and
falls back to your desktop's default audio application.

There is no waveform preview. It was 94 px of page height that the buttons
could use instead, and the drawing widget behind it was removed rather than left
built but invisible: it re-read the whole WAV and reduced 2,000 buckets on every
Convert, Play and Emulate, for something that could never be drawn.

### Stop

Stops playback. The player process is signalled, not merely forgotten, so the
sound really does stop.

### ->Emu

Writes a **TZX** file — a tape image holding a single direct-recording block —
instead of a WAV. Any Spectrum emulator that can play TZX can then load it
directly, with no need to route audio through the sound card.

Only available in *High speed* mode.

### Output directory

Where finished files are written. Leave it empty to write them alongside the
source file.

### LAME mp3 encoder, Encoder

Tick the box to enable the encoder options. `Encoder` is the program to run —
`lame` by default, with `ffmpeg` used automatically if `lame` fails. The entry
turns red if the named program is not on your `PATH`.

The bitrate is **320 kbps, fixed** — there is no control for it, and that is
deliberate. The original hard-coded 256; 320 is used here because it is the
highest rate the table below measures, and the ceiling of MPEG-1 Layer III, so
it is the last rate before the encoder starts costing something. The turbo
techniques put most of their energy above 7 kHz, which is the first band a lossy
encoder discards, so a lower rate would quietly damage the very signal the mp3
exists to preserve.

Note that this affects only the mp3 *copy*. The WAV is written first and is what
the Spectrum actually loads, so a lower bitrate would never break a load — but
it would make the archive you keep noticeably worse. If you would rather have a
different rate, convert to WAV and run `lame` yourself.


### -> WAV

Writes the WAV. This is the normal output and the one you want for loading a
real machine.

### -> MP3

Converts and then encodes to mp3 using the settings above. You do not need to
tick the **LAME mp3 encoder** box for this button — it is only about the
settings that follow it.

### Convert all...

Opens a batch window and converts every supported file in the current folder
one at a time, showing the program name, the recovered `USR` and `CLEAR`, the
block count, the duration and any warnings for each. Double-click a row to
convert just that one.

---

## 5. The About tab

### Overrides

| Field | When you need it |
|---|---|
| **CLEAR** | The file's BASIC program is loaded somewhere k7zx guessed wrong. Give the address the program expects. Leave empty to use what the file provided; the fallback is 65344 (`0xFF40`). |
| **USR** | Same, for the entry point. Leave empty to use the file's value, or `0` for "just load it, don't jump". |
| **Program name** | The name written into the generated tape header, up to 10 characters. |

These apply to the conversion immediately.

### Do pokes before loading

A **poke** writes a single byte into Spectrum memory at a chosen address
before the tape is played — the standard way to patch a game that needs a
single value changed to run.

Tick the box, then fill in **Address** and **Value** and press **Add** for
each byte you want changed. Values are decimal or `0x` hex, `0`–`65535` and
`0`–`255` respectively.

Pokes apply to the *loaded image*, not to the file on disk, so the file is
never modified. They take effect wherever k7zx builds the memory image itself —
both tape schemes and snapshots — but **not** under *As the original loader*,
where the program's own `LOAD` does the work.

---

## 6. Choosing a technique

You do not have to understand the encodings to use them. The short version:

* **Real Spectrum:** start with **Rayo at 2.75**, or **Shavings Raudo at 2.25**.
  If it will not load, step *up* the samples-per-bit list, or drop to **FSK at
  5.00**. (Raudo 2.75 is still the default setting, but the model rates it 🔴: it
  needs a clean line.)
* **Emulator:** anything loads, so pick for speed: **NPU at 1.25**, or **Rayo
  at 2.25** with compression.
* **MP3, streaming or cassette:** **FSK at 5.00–7.00**.

**Slower is not failure.** A 12,000 bps load is perfectly good.

The rest of this section gives the evidence. Every technique was tested at
every speed, in an emulated Spectrum and through a model of a real playback
chain. The per-speed results are in
[section 10](#10-the-techniques-in-detail).

### How to read the ratings

| | k7zx 4.3 column | Emulator column | Real Spectrum column |
|---|---|---|---|
| 🟢 | green in 4.3's speed list | loads with the tape's own perfect edges | **clean in all ten conditions, on both machines** |
| 🟡 | yellow in 4.3's speed list | — | — (4.3 had no rating of its own here) |
| 🟠 | — (4.3 had no orange) | — | clean in eight or nine of the ten |
| 🔴 | red in 4.3's speed list | does not load | clean in seven or fewer |

The Real Spectrum column is a single, complete measurement: every speed listed
below was put through **all ten** conditions `robustness.py` applies, on a 48K
with contention and on 128K timing, and the mark is the *worse* of the two. The
notes column names the conditions that did not produce a clean load, so nothing
is hidden behind the colour. There is no separate six-condition rating to
reconcile with: this table is one measurement, taken with the script as it
stands.

* **k7zx 4.3:** the colours the original program drew in its speed list, taken
  from its source code (`original/SettingdForm.cpp`). They are the author's own
  judgement, presumably from his own machines. This port does not draw them.
* **Emulator:** the WAV loaded into an emulated Spectrum that sees the WAV's
  edges perfectly, as an emulator does when it plays a WAV straight in. This was
  checked three ways:
  * on a 48K, at 44.1 and 48 kHz, in every loading scheme;
  * on a 48K with memory contention switched on;
  * on 128K timing (a clock 1.3 % faster).

  The emulator used is the port's own test machine (`tools/zxload`), not Fuse.
  Fuse also emulates contention and 128K timing, so it should behave similarly.
  The ✅/🔴 marks in the per-technique tables come from the `exact` row of the
  measurement below — one case, one block, 48 kHz, square wave. The wider sweep
  (44.1 and 48 kHz, all three schemes, Rayo included) is the evidence behind the
  prose in this section and is recorded separately in `tools/zxload/sweep-step15.txt`;
  it is not what the column is computed from.
* **Real Spectrum (model):** the WAV put through a model of a real playback
  chain (`tools/channel`) before loading. It runs on a 48K with contention and
  on 128K timing. The ten conditions the ratings below were measured with are:

The parameters below are the ones the script actually applies
(`CONDITIONS` in `tools/channel/robustness.py`), not an idealised set. Note
that there is no separate "clean" analogue condition: `exact` feeds the tape's
own edges to the emulator untouched, and the mildest modelled condition is a
threshold offset of 0.2 through a 20 kHz filter.

| Condition | Low-pass filter | Threshold offset | Other |
|---|---|---|---|
| exact | — | — | the tape's own edges, no analogue model at all |
| offset 0.2 | 20 kHz | 0.2 | the mildest modelled condition |
| offset 0.4 | 20 kHz | 0.4 | a badly aligned EAR comparator |
| inverted | 20 kHz | −0.2 | reversed polarity |
| 16 kHz | 16 kHz | 0.2 | a slightly dull output |
| 12 kHz | 12 kHz | 0.2 | a dull output, like a cheap adapter or a worn deck |
| 2 % fast | 20 kHz | 0.2 | tape or player 2 % fast |
| 2 % slow | 20 kHz | 0.2 | 2 % slow |
| noise 5 % | 20 kHz | 0.2 | 5 % noise |
| AC 200 Hz | 20 kHz | 0.2 | 200 Hz AC coupling |

All ten are used for every speed in the ratings below, and the mark is the worse
of the two machines. Only **six of the sixty-one** speed/technique combinations
come out clean in all ten: FSK 7.00, 6.00 and 5.00, Shavings Raudo 2.25, Ultra
3.00 and Escurrido 2.50. Anything marked 🟠 or 🔴 has its failing conditions
named in the notes column.

What these conditions mean:

* The **low-pass filter** stands for the sound card's or player's output
  filter and amplifier.
* The **threshold offset** is how far from the middle of the wave the EAR input
  flips. It is a fraction of the peak, and depends a lot on volume.
* Everything was tested at 48 kHz and with the **one-block** scheme, and
  `robustness.py` writes a **square** wave for every technique (`-w square -c
  one` are hard-coded) rather than each technique's default waveform. The test
  tape was `golden.tap`, a 10 KB program; this is not a game corpus.

**Read the hardware column as a strong hint, not a guarantee.** It comes from a
simulation, and nothing here was played into a real Spectrum. The model does
not cover cassette wow and flutter, MP3 artefacts, or differences between
Spectrum models. The "typical" condition's 0.2 offset is a judgement call. A
well-set volume can bring the real offset closer to "clean", which is probably
why some speeds the author rated green come out 🟠 here (see
[Where the tests disagree with k7zx 4.3](#where-the-tests-disagree-with-k7zx-43)).

### Recommendations

**For a real Spectrum, in order of how much margin the model shows:**

| Pick | Speed | bps @ 48 kHz | Rating | Why |
|---|---|---|---|---|
| **Rayo** | 2.75 | 17,454 before compression | 🟠 | Clean in nine of the ten; the 48K does not finish at a 12 kHz low-pass. It has a checksum, auto polarity and compression. |
| **Ultra** | 3.00 | 16,000 | 🟢 | Clean in every condition here, although 4.3 says it needs careful volume. |
| **FSK** | 5.00 | 9,600 | 🟢 | The most forgiving family, and the only one with three speeds that are clean in all ten conditions. 5.00 is the quickest of them; 4.00 is quicker still but never finishes at a badly aligned threshold, and 8.00 does so on the 48K. |
| **Shavings Delta** | 2.25–3.50 | up to 21,333 | 🟡 | The fastest original technique with a good margin. Polarity does not matter. |
| **Rayo** | 2.25 | 21,333 before compression | 🟡 | The fastest with margin. Its checksum reports nearly every bad load (but see Rayo's cons). |
| **Shavings Raudo** | 2.25 | 21,333 | 🟢 | Fast, well proven, and clean in all ten conditions at 2.25. It has no checksum, so a bad load runs corrupt rather than reporting itself. |
| **Escurrido** | 2.50 | 19,200 | 🟡 | A good margin at this speed only. |

Compression cuts a typical 48K game's whole load by about 30 %; how much
depends on how well the game compresses.

**For an emulator (Fuse, etc.), where only raw speed matters:**

| Pick | Speed | bps @ 48 kHz | Notes |
|---|---|---|---|
| **NPU** | 1.25 | 38,400 | The fastest raw rate that loads everywhere tested. It shows the loading screen as it loads. |
| **Escurrido** | 1.33 | 36,000 | Loads at 48 kHz only. |
| **Ultra** | 1.50 | 32,000 | Loads only as "many blocks". |
| **Rayo** | 2.25 | 21,333 raw | Compression can bring a whole load under NPU 1.25 for games that compress well, but there is no loading screen while compressing (see [Rayo](#rayo-added-by-this-port)). |


**Scheme limits to keep in mind.** Shavings Raudo and Rayo load one block only,
so *Many blocks* is disabled for them. *As the original loader* is for Milks
only. Rayo refuses 128K snapshots; use Shavings Raudo for those.

### MP3, streaming and cassette

k7zx writes a WAV or an MP3. It has no AAC output and never had, in this port
or in 4.3 — but you may well hand the WAV to an AAC encoder yourself for
streaming or Bluetooth, and lossy audio throws away the high frequencies the
fast techniques depend on, so it is worth knowing what survives each rate.

To see that, `golden.tap` was converted at 44.1 kHz, encoded, decoded again and
loaded on an emulated 48K. Each file was loaded twice: from its own edges, as an emulator
sees it, and through the "clean line" model of real hardware. The WAV here is
44.1 kHz, one-block scheme, **cubic** waveform — the port's default — whereas
the ratings in §10 are square (`robustness.py` hard-codes `-w square`); the two
tables are separate experiments and are not comparable cell for cell.

| | Meaning |
|---|---|
| ✅ | loads with no wrong byte at all, both ways |
| ⚠️ | loads both ways, but with wrong bytes in at least one |
| ❌ | does not load at least one way |

The table below was produced by `tools/channel/mp3_matrix.py`, which is how to
check it: run it and it prints the same rows, with the bit-rate column this
table adds for reference.

| Technique | bps @ 44.1 kHz | WAV | MP3 320 | MP3 192 | MP3 128 | MP3 96 |
|---|---|---|---|---|---|---|
| FSK 8.00 | 5,512 | ✅ | ✅ | ✅ | ✅ | ❌ |
| FSK 5.00 | 8,820 | ✅ | ✅ | ✅ | ✅ | ❌ |
| FSK 4.00 | 11,025 | ✅ | ✅ | ✅ | ⚠️ | ❌ |
| Fi 6.00 | 7,350 | ✅ | ✅ | ✅ | ✅ | ⚠️ |
| Manchester 4.00 | 11,025 | ✅ | ✅ | ✅ | ⚠️ | ❌ |
| Shavings Slow 4.00 | 11,025 | ✅ | ✅ | ✅ | ⚠️ | ❌ |
| Shavings Delta 3.50 | 12,600 | ✅ | ✅ | ✅ | ❌ | ❌ |
| Shavings Raudo 2.75 | 16,036 | ✅ | ✅ | ✅ | ⚠️ | ❌ |
| Rayo 2.75 | 16,036 | ✅ | ✅ | ✅ | ❌ | ❌ |
| NPU 1.25 | 35,280 | ❌ | ❌ | ❌ | ❌ | ❌ |


What this means in practice:

* **For an MP3 file — or any other lossy encode of the WAV — use FSK and keep
  the bitrate at 128 kbps or more.** Of the ten rows measured, only FSK 8.00,
  FSK 5.00 and Fi 6.00 are ✅ at 128 kbps. Below 128 kbps almost nothing loads,
  and NPU does not survive lossy encoding at any rate tested.
* **Rayo needs 192 kbps or more** to come out clean, and everything else here
  needs 128. The **-> MP3** button encodes at 320 kbps, the top of the tested
  range, so its own output is never the problem.
* **For a cassette, use FSK at 5.00–7.00.** FSK 4.00 survives the bitrate test
  but does not finish at a badly aligned threshold, and Fi at 6.00–8.00 each fail
  at least one of the ten conditions `robustness.py` applies, so none of them is
  the first thing to reach for. The model does not include tape wow and flutter,
  so treat that as a starting point.

k7zx 4.3 had a separate low-rate mode, "Ma non troppo", for exactly this job.
It was removed from this port because FSK did as well or better in these tests
while loading faster. This was one small test program and one encoder of each
kind, so the borderline cells are approximate.

---

## 7. Loading a tape for real

### Into an emulator

Simplest. Point the emulator at the WAV (or the TZX from **->Emu**) and
select the right machine. Make sure the emulator is set to read the audio input
device, not the microphone.

### On real hardware, from a computer

1. Connect the Spectrum's **EAR** socket to your sound card's **line out** —
   not headphone out, and never a microphone input.
2. Set the sound card's recording level so the meter peaks around two-thirds;
   clipping will corrupt the signal.
3. In k7zx, pick the technique and speed, press **Play** to check the level
   with a meter, then **-> WAV**.
4. On the Spectrum, type `LOAD ""` and start playback within a second or two of
   the leader beginning.

### On real hardware, onto a cassette

1. Pick **FSK at 5.00–7.00** and press **-> WAV**. Use the WAV, not an
   MP3, as the source.
2. Record that file onto tape in mono, from a **line-level** source.
3. Load with `LOAD ""`.

### Things that help when a real machine refuses

- **Tick Final tone** — gives the deck's AGC something to latch onto.
- **Try a different waveform** — cubic first, then `Continuous Compensate`.
- **Tick Invert wave** — some decks read the opposite polarity.
- **Step down the speed** by one notch: pick the next *higher* samples-per-bit
  value.
- **Check the volume.** Most techniques are sensitive to where the Spectrum's
  input flips relative to the middle of the wave, and that moves with volume.
  The techniques rated 🟠 in section 10 only load when it is well set.
- **Switch to stereo** and, if the noise is in one channel, tick
  **Reverse right channel**.
- On a **128K** machine the clock is about 1.3 % faster; a marginal setting
  that fails on 48K may work on 128K and vice versa. Section 10 notes where the
  two differ.

---

## 8. Command line

`k7zx-cli` exposes the conversion options the GUI exposes, for scripts, and it
does apply pokes. It has no audio playback and no `--self-test`: those are
`k7zx` only.

```sh
# a 17 kbps Raudo load (the default technique and speed)
k7zx-cli -m hispeed -t raudo -s 2.75 game.tap

# Rayo with compression (the default for Rayo) and the fast BASIC loader
k7zx-cli -t rayo -s 2.25 --accelerate game.z80

# inspect what is in a file
k7zx-cli -v game.tap

# a verbatim replay
k7zx-cli -m normal game.tzx out.wav

# an mp3 for archiving
k7zx-cli -t fsk -s 5.00 game.tap && lame -b 192 game_FSK*_5.00.wav out.mp3

# a TZX for an emulator
k7zx-cli --emulate -t fiq -s 2.5 game.tap out.tzx

# play without a window (this is the graphical k7zx, not k7zx-cli)
k7zx -p -t fsk -s 5.00 game.tap
```

### Options

| Option | Meaning |
|---|---|
| `-m, --mode` | `normal` or `hispeed` (default `hispeed`) |
| `-t, --method` | Technique name: `rom` `milks` `fsk` `slow` `delta` `raudo` `ultra` `npu` `fi` `fiq` `manchester` `manchester-dif` `escurrido` `rayo` |
| `-s, --spb` | Samples per bit: `2.75`, `275`, or `1.33` / `133`. It must be a speed the chosen technique actually offers — see the table in section 11. The command line refuses anything else rather than write a wave that cannot load. |
| `-r, --rate` | 44100 or 48000 |
| `-w, --waveform` | `square` `ramp` `cubic` `continuous` `energy` `parabola` `delta` |
| `-c, --scheme` | `one`, `many` or `original` |
| `--stereo` | 16-bit stereo instead of 8-bit mono |
| `--invert`, `--invert-right` | Polarity |
| `--final-tone` | Append the decaying closing tone |
| `--accelerate` | Fast BASIC loader |
| `--no-checksum` | Do not install the tape error handler |
| `--no-loader` | Do not synthesise the BASIC loader |
| `--kolmogorov` | Statistical Delta timing |
| `--compress`, `--no-compress` | Rayo: LZ compression on (default) or off |
| `--clear`, `--usr`, `--name` | Override the recovered values |
| `--blocks` | Comma-separated block indices, or `all` / `none` |
| `--poke ADDR=VALUE` | Poke a byte before loading; repeatable. Base 0, so `0x5ce5` works as well as `23781`. Same as the GUI's *Do pokes before loading* box. |
| `--emulate` | Write TZX instead of WAV |
| `--mp3` | Also encode the result to mp3. The WAV is written first and removed once the encode succeeds. |
| `--mp3-encoder PROG` | Encoder to run: `lame`, or `$K7ZX_MP3_ENCODER`, with `ffmpeg` as a fallback. Implies `--mp3`. |
| `--mp3-bitrate KBPS` | Bit rate to ask for, default 320. Implies `--mp3`. |
| `-v, --verbose` / `-q, --quiet` | Report detail / errors only |
| `--config-path` | Use a different `k7zx.ini` |

`--compress` is Rayo-only, as in the GUI: given for any other technique the
command line says so and carries on uncompressed, rather than accepting a flag
that quietly does nothing.

`--help` prints the same list, including `--compress`.

---

## 9. Troubleshooting

**Nothing loads at all.**
Check the level first with **Play** and a meter. If the meter moves but the
Spectrum hears nothing, the output is going to the wrong socket or a microphone
input. Then tick **Invert wave** — some decks read the opposite polarity.

**It loads, then crashes with a scrolling border.**
A block is overwriting the system variables. Look for the red *"Block
overrides address $ff3c"* warning, untick that block, or raise `CLEAR`.

**The program loads but nothing happens.**
The `USR` entry point is wrong. Set it on the **About** tab, or untick the
program's block if you only want the data.

**`No entry point found` style behaviour on a `.tap`.**
The file has data blocks without headers, or no recoverable `CLEAR`/`USR`. Set
both on the **About** tab, and use **All in one block**.

**A fast technique will not load.**
Step **up** the samples-per-bit list, or pick a technique rated 🟢 or 🟡 for
real hardware in section 10. Reliable at 12,000 bps is a success, not a
failure.

**It loads in the emulator but not on the real machine.**
Expected for the fastest speeds: an emulator sees perfect edges, and a real
sound card and amplifier do not deliver them. NPU at 1.25, for example, loads
in an emulator and fails every real-hardware test. See section 10.

**Rayo shows no loading screen.**
That is compression: the screen only appears when the data is expanded at the
end. Untick **Compress (Rayo)** or use `--no-compress`.

**The file is quiet when played through speakers.**
Expected. The fast techniques put 69–89 % of their energy above 7 kHz, which
small speakers and laptop audio discard. It does not affect loading — the
Spectrum reads edges, not loudness. If you want to *listen*, step the
samples-per-bit up to 5.00 or so; that brings the energy down into the audio
band without changing the technique.

**mp3 is larger than the WAV, or sounds thin.**
You used a fast technique. A lossy codec throws away the high-frequency
content. Switch to **FSK at 5.00** and it will compress properly.

**"Wave properties needs scrolling".**
The options no longer fit the row. The self-test reports the exact numbers.

**Settings do not persist.**
Check the file is writable:
`ls -l ~/.config/k7zx/k7zx.ini`

---

## 10. The techniques in detail

Each technique below has a short description, its pros and cons, and a table
with one row per speed. The columns and colours are explained in
[How to read the ratings](#how-to-read-the-ratings). In the notes a condition is named as
`robustness.py` prints it — `exact`, `offset 0.2`, `offset 0.4`, `inverted`,
`16 kHz`, `12 kHz`, `2% fast`, `2% slow`, `noise 5%` and `AC 200 Hz` — and all
ten are used for every speed. "`48K only:`" or "`128K only:`" means only that
machine failed; "loads with wrong bytes at" means it finished but some bytes were
wrong; "fails" means it never finished at all.

### Summary

| Technique | Fastest 🟢 on real hardware (model) | Fastest that loads in an emulator | Error check | Many blocks | Polarity-independent |
|---|---|---|---|---|---|
| ROM | none (best is 3.00, 4/10) | 3.00 (16,000) | not offered | yes | either |
| Milks | none (best is 2.75, 1/10) | 2.75 (17,454) | optional | yes, plus "as the original loader" | 4.3 says 3.50 does; it does not |
| FSK | 5.00 (9,600) 🟢 | 2.50 (19,200) | optional | yes | — |
| Shavings Slow | none (best is 3.00, 9/10) | 2.50 (19,200) | optional | yes | — |
| Shavings Delta | none (best is 2.25, 8/10) | 1.75 (27,428) | not offered | yes | either |
| Shavings Raudo | 2.25 (21,333) 🟢 | 1.75 (27,428) | not offered | no | — |
| Ultra | 3.00 (16,000) 🟢 | 2.00 (24,000) | not offered | yes | — |
| NPU | none (best is 1.25, 1/10) | 1.25 (38,400) | optional | yes | — |
| Fi | none (best is 8.00, 9/10) | 3.00 (16,000) | optional | yes | — |
| Fi Quadruple | none (best is 3.50, 6/10) | 1.75 (27,428) | not offered | yes | — |
| Manchester | none (best is 4.00, 9/10) | 2.00 (24,000) | optional | yes | — |
| Man. diferencial | none (best is 4.00, 9/10) | 2.00 (24,000) | optional | yes | either (same text as Manchester) |
| Escurrido | 2.50 (19,200) 🟢 | 1.33 (36,000) | not offered | yes | — |
| **Rayo** (port addition) | none (best is 2.25, 9/10) | 2.25 (21,333 raw) | built in (XOR + end check) | no (one block) | learnt from the sync |

Notes on the summary:

* **Error check.** "Optional" means the *Verify tape loading error* option is
  available: a checksum failure then reports an error instead of crashing.
  "Not offered" means 4.3 greyed that option out for the technique. Without a
  check, a bad load usually crashes or runs a corrupt program.
* **Polarity-independent.** "Either" means 4.3's own description says it loads
  whatever the signal's polarity. "—" means it does not say; if one of those
  will not load, try *Invert*. The Manchester and Man. diferencial entries share
  one description, so both are marked the same way.
* **Milks at 3.50** is listed as "3.50 only works either way" because 4.3 says so,
  but the measurement disagrees: it fails an inverted signal on both machines.
  The per-speed table below is the one to trust.

### ROM

**How it works.** Each bit is the length of one pulse, as in the Spectrum's
own ROM loader, but with the timings squeezed. The routine is the simplest of
all.

**Pros.** It uses the Spectrum's own loading principle, and works with either
polarity when the two halves of each pulse are equal. It works on any model.

**Cons.**
* Only two speeds.
* Timing a single pulse is sensitive to the EAR threshold.
* 3.00 needs a clean line, and 2.50 does not load at all.

| Speed | bps @ 48 kHz | k7zx 4.3 | Emulator | Real Spectrum (model) | Notes |
|---|---|---|---|---|---|
| 3.00 | 16,000 | 🟡 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, inverted, noise 5%, offset 0.4; 48K only: 2% fast |
| 2.50 | 19,200 | 🔴 | 🔴 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, exact, inverted, noise 5%, offset 0.2, offset 0.4 |
### Milks

**How it works.** It is the ROM principle with 2 bits per pulse. It is the only
technique with a per-block header (address and length), so it can emulate
`LOAD "" CODE`. It is also the only one that offers the "as the original
loader" scheme.

**Pros.**
* Keeps a program's original block structure.
* k7zx 4.3 documents 3.50 as working with either polarity. The measurement does
  not bear that out: 3.50 fails an inverted signal on both machines, and picks
  up wrong bytes at a typical threshold offset. Treat 2.75 as the usable speed.

**Cons.**
* Timing half-pulses makes it sensitive to threshold offset: at a typical
  offset the 48K loads 854 bytes wrong even at 3.50.
* 2.50, 2.25 and 1.75 do not load at 48 kHz even in an emulator; use 44.1 kHz.

| Speed | bps @ 48 kHz | k7zx 4.3 | Emulator | Real Spectrum (model) | Notes |
|---|---|---|---|---|---|
| 3.50 | 13,714 | 🟢 | 🟢 | 🔴 | fails inverted, offset 0.4; 48K only: 12 kHz, 16 kHz, 2% fast, noise 5%; loads with wrong bytes at 12 kHz, 2% slow, AC 200 Hz, offset 0.2 |
| 2.75 | 17,454 | 🟡 | 🟢 | 🔴 | fails 12 kHz, AC 200 Hz, inverted, offset 0.4; 48K only: 16 kHz, 2% fast, noise 5%, offset 0.2; loads with wrong bytes at 16 kHz, 2% fast, 2% slow, noise 5%, offset 0.2 |
| 2.50 | 19,200 | 🔴 | 🔴 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, exact, inverted, noise 5%, offset 0.2; 128K only: offset 0.4; loads with wrong bytes at offset 0.4 |
| 2.25 | 21,333 | 🟡 | 🔴 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, exact, inverted, noise 5%, offset 0.2, offset 0.4 |
| 1.75 | 27,428 | 🔴 | 🔴 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, exact, inverted, noise 5%, offset 0.2, offset 0.4 |
### FSK

**How it works.** One bit per full wave cycle, a short cycle or a long one. It
uses the default "cubic" waveform.

**Pros.**
* Full-cycle timing cancels threshold offset, so it is the most forgiving
  family.
* 5.00 to 7.00 are clean in every condition. It is good for cassettes and
  poor players.
* **But 4.00 and 8.00 are not.** Re-measured through
  all ten that `robustness.py` applies, 4.00 never finishes when the
  threshold is offset by 0.4, on either machine, and 8.00 does so on the 48K.
  5.00, 6.00 and 7.00 are clean throughout. That is why the advice
  throughout this guide is FSK at 5.00–7.00 rather than 4.00.

**Cons.**
* One bit per cycle makes it slow.
* At 3.00 and below the cycles get short enough that filtering starts to bite.

| Speed | bps @ 48 kHz | k7zx 4.3 | Emulator | Real Spectrum (model) | Notes |
|---|---|---|---|---|---|
| 8.00 | 6,000 | 🟢 | 🟢 | 🟠 | 48K only: offset 0.4 |
| 7.00 | 6,857 | 🟢 | 🟢 | 🟢 |  |
| 6.00 | 8,000 | 🟢 | 🟢 | 🟢 |  |
| 5.00 | 9,600 | 🟢 | 🟢 | 🟢 |  |
| 4.00 | 12,000 | 🟢 | 🟢 | 🟠 | fails offset 0.4 |
| 3.00 | 16,000 | 🟢 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, noise 5%, offset 0.4 |
| 2.50 | 19,200 | 🔴 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, noise 5%, offset 0.2, offset 0.4; 48K only: inverted; loads with wrong bytes at inverted |
### Shavings Slow

**How it works.** Like FSK, but each cycle carries 2 bits, using four cycle
lengths.

**Pros.**
* Twice FSK's density at the same speed.
* Holds up much better on 128K timing than on a contended 48K.

**Cons.**
* On the 48K with contention it loses margin quickly. At 3.50 and faster it
  fails at a badly aligned threshold on both machines, which 4.3's all-green
  rating does not suggest.

| Speed | bps @ 48 kHz | k7zx 4.3 | Emulator | Real Spectrum (model) | Notes |
|---|---|---|---|---|---|
| 4.00 | 12,000 | 🟢 | 🟢 | 🟠 | fails offset 0.4 |
| 3.50 | 13,714 | 🟢 | 🟢 | 🔴 | 48K only: 2% fast, AC 200 Hz, inverted, noise 5%, offset 0.2; loads with wrong bytes at 12 kHz |
| 3.00 | 16,000 | 🟢 | 🟢 | 🟠 | fails offset 0.4 |
| 2.50 | 19,200 | 🟢 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, noise 5%, offset 0.4; loads with wrong bytes at 2% fast |
### Shavings Delta

**How it works.** It is the 2-bits-per-cycle family. The positive pulse has a
fixed length of 1 or 2 samples, and the data is in the length of the rest of
the cycle. It has a *Statistical optimisation* option that puts the commonest
bit pairs on the shortest cycles.

**Pros.**
* Works with either polarity, with no detection code.
* With Raudo 2.25, the best margin of any original technique at fast speeds:
  2.25 (21,333 bps) passes the threshold offset on both machines.

**Cons.**
* 4.3 offers no error check for it.
* Some 2 % slow failures on the 128K.
* 1.75 only works on a clean line.

| Speed | bps @ 48 kHz | k7zx 4.3 | Emulator | Real Spectrum (model) | Notes |
|---|---|---|---|---|---|
| 3.50 | 13,714 | 🟢 | 🟢 | 🟠 | fails offset 0.4; loads with wrong bytes at 2% slow |
| 3.00 | 16,000 | 🟢 | 🟢 | 🟠 | fails 12 kHz, offset 0.4 |
| 2.75 | 17,454 | 🟢 | 🟢 | 🟠 | fails offset 0.4; loads with wrong bytes at 2% slow |
| 2.25 | 21,333 | 🟡 | 🟢 | 🟠 | loads with wrong bytes at 2% slow, noise 5% |
| 1.75 | 27,428 | 🔴 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, inverted, noise 5%, offset 0.4; 48K only: 2% fast; loads with wrong bytes at 2% slow, AC 200 Hz, offset 0.2 |
### Shavings Raudo

**How it works.** Two bits per full cycle, with cycles of 3 to 6 samples at
2.25. The loop is the fastest possible (`in l,(c) / jp (hl)` with the R
register as a clock).

**Pros.**
* Fast and well proven.
* Full-cycle timing means 2.25 and 2.75 hold up to the clean and typical
  conditions.

**Cons.**
* No checksum, so a bad load is never reported. Several failures here ran to
  the end with wrong bytes in memory.
* One block only, and not for 128K multi-block.
* 2.50 does worse than 2.25: its cycles alternate between fractional lengths,
  which costs margin.
* 1.75 uses 2-sample cycles that do not survive any filtering.

| Speed | bps @ 48 kHz | k7zx 4.3 | Emulator | Real Spectrum (model) | Notes |
|---|---|---|---|---|---|
| 2.75 | 17,454 | 🟢 | 🟢 | 🔴 | loads with wrong bytes at 12 kHz, 16 kHz, 2% fast |
| 2.50 | 19,200 | 🟢 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, noise 5%, offset 0.4 |
| 2.25 | 21,333 | 🟢 | 🟢 | 🟢 |  |
| 1.75 | 27,428 | 🟡 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, noise 5%, offset 0.2, offset 0.4; loads with wrong bytes at inverted |
### Ultra

**How it works.** The negative half and the positive half of each cycle each
carry one bit, so the edges themselves are the data.

**Pros.**
* At 3.00 it was clean in every condition, better than its reputation.
* Quite fast in an emulator.

**Cons.**
* Half-cycle timing is offset-sensitive, so 2.00 needs a clean line. 4.3 says
  it needs fine volume adjustment and the right waveform.
* 2.50 and 1.50 load only as "many blocks", even in an emulator. As one block
  they fail, and that is 4.3's own behaviour.

| Speed | bps @ 48 kHz | k7zx 4.3 | Emulator | Real Spectrum (model) | Notes |
|---|---|---|---|---|---|
| 3.00 | 16,000 | 🟢 | 🟢 | 🟢 |  |
| 2.50 | 19,200 | 🔴 | 🔴 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, exact, inverted, noise 5%, offset 0.2, offset 0.4 |
| 2.00 | 24,000 | 🟡 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, noise 5%, offset 0.4; 48K only: AC 200 Hz; loads with wrong bytes at 2% fast, 2% slow, AC 200 Hz, inverted, offset 0.2 |
| 1.50 | 32,000 | 🔴 | 🔴 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, exact, inverted, noise 5%, offset 0.2, offset 0.4 |
### NPU

**How it works.** Ultra with 2 bits per half-cycle: four half-pulse lengths.

**Pros.**
* The fastest raw rate in k7zx: 38,400 bps at 1.25.
* Loads in emulators at every speed except 1.75 on a contended 48K.
* Shows the loading screen as it loads.

**Cons.**
* Very short half-pulses timed one by one. On real hardware the model shows
  hundreds of wrong bytes from filtering alone, even with a perfectly centred
  threshold.
* It is an emulator-only technique except perhaps at 2.00 on a very clean line.

| Speed | bps @ 48 kHz | k7zx 4.3 | Emulator | Real Spectrum (model) | Notes |
|---|---|---|---|---|---|
| 2.50 | 19,200 | 🟡 | 🟢 | 🔴 | fails offset 0.4; 128K only: AC 200 Hz, inverted; loads with wrong bytes at 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, inverted, noise 5%, offset 0.2 |
| 2.00 | 24,000 | 🟡 | 🟢 | 🔴 | fails 12 kHz, 2% fast, inverted, noise 5%; 128K only: 16 kHz, 2% slow, AC 200 Hz, offset 0.4; loads with wrong bytes at 16 kHz, 2% slow, AC 200 Hz, offset 0.2, offset 0.4 |
| 1.75 | 27,428 | 🔴 | 🔴 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, offset 0.2, offset 0.4; 48K only: exact, inverted, noise 5%; loads with wrong bytes at inverted, noise 5% |
| 1.25 | 38,400 | 🔴 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, inverted, noise 5%, offset 0.2, offset 0.4; 128K only: AC 200 Hz; loads with wrong bytes at AC 200 Hz |
### Fi

**How it works.** The cycle length is constant, and each bit is in the
difference between the positive and negative halves (the duty cycle). One
routine loads every speed.

**Pros.**
* 8.00 comes closest: it has a single soft spot, a badly aligned threshold.
  6.00 and 7.00 each fail two conditions.
* Simple and flexible.

**Cons.**
* A threshold offset shifts the duty cycle directly, which is exactly where the
  data is. So from 5.00 down it needs a clean line, and 3.00 fails even then.

| Speed | bps @ 48 kHz | k7zx 4.3 | Emulator | Real Spectrum (model) | Notes |
|---|---|---|---|---|---|
| 8.00 | 6,000 | 🟢 | 🟢 | 🟠 | loads with wrong bytes at offset 0.4 |
| 7.00 | 6,857 | 🟢 | 🟢 | 🟠 | fails offset 0.4; 48K only: noise 5%; loads with wrong bytes at noise 5% |
| 6.00 | 8,000 | 🟢 | 🟢 | 🔴 | fails noise 5%, offset 0.4; 128K only: 12 kHz |
| 5.00 | 9,600 | 🟡 | 🟢 | 🔴 | 48K only: 12 kHz, offset 0.4; loads with wrong bytes at 12 kHz, 16 kHz, 2% fast, AC 200 Hz, inverted, noise 5%, offset 0.4 |
| 4.00 | 12,000 | 🟡 | 🟢 | 🔴 | fails AC 200 Hz; 128K only: 12 kHz, 16 kHz, 2% fast, 2% slow, noise 5%, offset 0.2; loads with wrong bytes at 12 kHz, 16 kHz, 2% fast, 2% slow, noise 5%, offset 0.2, offset 0.4 |
| 3.00 | 16,000 | 🔴 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, noise 5%, offset 0.2, offset 0.4; loads with wrong bytes at inverted |
### Fi Quadruple

**How it works.** Fi with 2 bits per cycle: four duty-cycle levels.

**Pros.** Fast in an emulator, down to 1.75 (27,428 bps).

**Cons.**
* Four duty levels are even more offset-sensitive than Fi's two. Even 3.50
  fails at a badly aligned threshold on both machines, and at 12 kHz and 16 kHz
  on the 48K.
* 4.3 offers no error check for it.

| Speed | bps @ 48 kHz | k7zx 4.3 | Emulator | Real Spectrum (model) | Notes |
|---|---|---|---|---|---|
| 3.50 | 13,714 | 🟢 | 🟢 | 🔴 | fails offset 0.4; 48K only: 12 kHz, 16 kHz; loads with wrong bytes at 12 kHz, noise 5% |
| 3.00 | 16,000 | 🟡 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, offset 0.4; 48K only: AC 200 Hz, noise 5%, offset 0.2; 128K only: 2% slow; loads with wrong bytes at 2% slow, AC 200 Hz, inverted, noise 5%, offset 0.2 |
| 2.75 | 17,454 | 🟡 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, offset 0.4; 48K only: AC 200 Hz, noise 5%, offset 0.2; 128K only: 2% slow; loads with wrong bytes at 2% fast, 2% slow, AC 200 Hz, inverted, noise 5% |
| 2.50 | 19,200 | 🟡 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, noise 5%, offset 0.2, offset 0.4 |
| 2.25 | 21,333 | 🔴 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, noise 5%, offset 0.2, offset 0.4; loads with wrong bytes at inverted |
| 2.00 | 24,000 | 🔴 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, noise 5%, offset 0.2, offset 0.4; loads with wrong bytes at inverted |
| 1.75 | 27,428 | 🔴 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, noise 5%, offset 0.4; 128K only: offset 0.2; loads with wrong bytes at inverted, offset 0.2 |
### Manchester

**How it works.** Clocked, with each bit as a low-high or high-low transition
at a fixed rate. The default waveform is "equal energy".

**Pros.**
* Steady, DC-free signal.
* 4.00 holds up well.
* Worth trying on a deck or player that struggles with others.

**Cons.**
* 3.00 needs a clean line, and 2.00 fails even there.
* Slower than the 2-bits-per-cycle families at the same margin.

| Speed | bps @ 48 kHz | k7zx 4.3 | Emulator | Real Spectrum (model) | Notes |
|---|---|---|---|---|---|
| 4.00 | 12,000 | 🟢 | 🟢 | 🟠 | loads with wrong bytes at 12 kHz |
| 3.00 | 16,000 | 🟢 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, offset 0.4 |
| 2.00 | 24,000 | 🔴 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, inverted, noise 5%, offset 0.2, offset 0.4 |
### Man. diferencial

**How it works.** Differential Manchester: each bit is whether the level flips
at the start of its slot, so polarity does not matter.

**Pros.**
* Polarity-independent by design.
* Otherwise like Manchester; at 3.00 it did slightly better.

**Cons.** As Manchester.

| Speed | bps @ 48 kHz | k7zx 4.3 | Emulator | Real Spectrum (model) | Notes |
|---|---|---|---|---|---|
| 4.00 | 12,000 | 🟢 | 🟢 | 🟠 | 128K only: 12 kHz; loads with wrong bytes at 12 kHz |
| 3.00 | 16,000 | 🟢 | 🟢 | 🟠 | fails 12 kHz; 48K only: offset 0.4; loads with wrong bytes at offset 0.4 |
| 2.00 | 24,000 | 🔴 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, inverted, noise 5%, offset 0.2, offset 0.4 |
### Escurrido

**How it works.** Works like Shavings Delta, but with a more complicated
encoding to go faster.

**Pros.**
* 2.50 (19,200 bps) has a good real-hardware margin.
* Very fast in an emulator: 1.33 gives 36,000 bps.

**Cons.**
* 4.3 offers no error check for it.
* 2.25 needs a clean line; 2.00, 1.50 and 1.33 fail even there.
* In an emulator, 1.33 loads at 48 kHz only.
* 1.50 and 2.00 have problems on a contended 48K.

| Speed | bps @ 48 kHz | k7zx 4.3 | Emulator | Real Spectrum (model) | Notes |
|---|---|---|---|---|---|
| 2.50 | 19,200 | 🟢 | 🟢 | 🟢 |  |
| 2.25 | 21,333 | 🔴 | 🟢 | 🔴 | fails 12 kHz, 16 kHz, AC 200 Hz, inverted, noise 5%, offset 0.4; 48K only: 2% fast, offset 0.2 |
| 2.00 | 24,000 | 🔴 | 🔴 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, AC 200 Hz, inverted, noise 5%, offset 0.2, offset 0.4; 48K only: 2% slow; loads with wrong bytes at exact |
| 1.50 | 32,000 | 🔴 | 🔴 | 🔴 | fails 12 kHz, 16 kHz, 2% fast, 2% slow, AC 200 Hz, noise 5%, offset 0.2, offset 0.4; 48K only: exact, inverted; loads with wrong bytes at inverted |
| 1.33 | 36,000 | 🔴 | 🟢 | 🔴 | fails 12 kHz, 2% fast, 2% slow, AC 200 Hz, inverted, noise 5%, offset 0.2, offset 0.4; 128K only: 16 kHz; loads with wrong bytes at 16 kHz |
### Rayo (added by this port)

**How it works.** Raudo's signal: 2 bits per full cycle, at 2.25 or 2.75. The
loader adds:

* an XOR checksum and an end-address check;
* polarity learnt from the sync;
* the port read so that held keys are ignored;
* a per-tape mapping of bit pairs to cycle lengths;
* optional LZ compression, expanded in place after loading.

**Pros.**
* 2.75 is the fastest speed measured with only one soft spot: clean in nine of
  the ten conditions, the 48K failing only at a 12 kHz low-pass.
* 2.25 gives up one condition to Raudo's 2.25 — a 5 % noise test on 128K timing —
  but adds an error check on top.
* Compression shortens a typical 48K game's total load by about 30 % against
  Raudo 2.25.
* In every other failure seen, Rayo stopped instead of running the program.

**Cons.**
* **The XOR checksum is not airtight in principle.** Two mistakes that flip the
  same bit cancel in an XOR. In the ten-condition matrix this has not produced a
  silent bad load — Rayo 2.25's one 128K failure is rejected outright — but a CRC
  or an additive checksum would close the gap.
* There is **no loading screen while compressing**. The compressed data loads
  high in memory and is only expanded into place at the end.
* It loads one block, which must end below 64832 (`$FD40`), and refuses 128K
  snapshots.
* It is not faster than NPU 1.25 in an emulator unless the game compresses
  well.

| Speed | bps @ 48 kHz | k7zx 4.3 | Emulator | Real Spectrum (model) | Notes |
|---|---|---|---|---|---|
| 2.75 | 17,454 before compression | — | 🟢 | 🟠 | 48K only: 12 kHz |
| 2.25 | 21,333 before compression | — | 🟢 | 🟠 | 128K only: noise 5% |
### Where the tests disagree with k7zx 4.3

The model mostly agrees with the original colours on what is red. It is
harsher on some speeds the author rated green or yellow:

* **Half-pulse and duty-cycle techniques** — Milks at every speed, Fi 4.00–5.00, Fi
  Quadruple at every speed, Ultra 2.00 and Shavings Slow at 3.50 and 2.50 — come out
  🔴 where 4.3 said green or yellow.
  They time individual half-cycles, so a threshold offset shifts their timing
  directly. The model's "typical" offset of 0.2 is enough to break them. At a
  well-set volume they may do as 4.3 says.
* **FSK 3.00 and Shavings Slow 3.50 / 3.00 / 2.50**, rated green by 4.3, need a
  clean line in the model.
* **Ultra 3.00** comes out better than its reputation (🟢).
* **Shavings Delta 2.25**, which 4.3 rated yellow, is one of the best fast options
  in the model (🟠 — clean in eight of the ten; it picks up wrong bytes at 2 % slow
  and at 5 % noise).

Several speeds that 4.3 rated yellow or red do not load even in an emulator
in some setups: Milks 2.50 / 2.25 / 1.75 at 48 kHz, Ultra 2.50 / 1.50 as one
block, and Escurrido at 44.1 kHz. In every case the port's output is
byte-identical to 4.3's own. Those are limits of the original, reproduced
faithfully.

### Reproducing the tests

```sh
# emulator: every technique × speed × rate × scheme
sh tools/zxload/sweep.sh build/k7zx-cli build/zxload/zxload

# analogue chain: wrong bytes per condition, 48K and 128K
python3 tools/channel/robustness.py build/k7zx-cli build/zxload/zxload \
    tests/data/golden.tap rayo:2.25 npu:1.25 fsk:4.00
```

`tools/README.md` describes both tools. The ratings come from the ten rows
`robustness.py` prints: `exact`, which is the *emulator* column, and the nine
analogue conditions whose parameters are in the table in
[section 6](#how-to-read-the-ratings).

---

## 11. Reference tables

### The tape format, for context

The Spectrum ROM writes each block as:

- a **leader** of 2168-T-state pulses — 8064 for a header block, 3220 for data
- a **sync** pair of 667 then 735 T-states
- the **data**, most significant bit first, a bit being two pulses of 855
  T-states for a `0` and two of 1710 for a `1`
- a **checksum** byte, chosen so that XORing every byte of the block including
  the flag byte gives zero

A tape **header block** is 19 bytes: flag, type, 10-character name, data
length, parameter 1 and parameter 2, then the checksum. Type 0 is a BASIC
program, 1 a number array, 2 a character array, 3 code. For a program,
parameter 1 is the `LINE` to run and parameter 2 the offset of its variables.

All timings are in T-states at 3.5 MHz, so one T-state is 1/3,500,000 s.

### Turbo techniques and their speeds

| Technique | `-t` name | Speeds (samples/bit) | Fastest bps @ 48 kHz |
|---|---|---|---|
| ROM | `rom` | 3.00, 2.50 | 19,200 |
| Milks | `milks` | 3.50, 2.75, 2.50, 2.25, 1.75 | 27,428 |
| FSK | `fsk` | 8.00, 7.00, 6.00, 5.00, 4.00, 3.00, 2.50 | 19,200 |
| Shavings Slow | `slow` | 4.00, 3.50, 3.00, 2.50 | 19,200 |
| Shavings Delta | `delta` | 3.50, 3.00, 2.75, 2.25, 1.75 | 27,428 |
| Shavings Raudo | `raudo` | 2.75, 2.50, 2.25, 1.75 | 27,428 |
| Ultra | `ultra` | 3.00, 2.50, 2.00, 1.50 | 32,000 |
| NPU | `npu` | 2.50, 2.00, 1.75, 1.25 | 38,400 |
| Fi | `fi` | 8.00, 7.00, 6.00, 5.00, 4.00, 3.00 | 16,000 |
| Fi Quadruple | `fiq` | 3.50, 3.00, 2.75, 2.50, 2.25, 2.00, 1.75 | 27,428 |
| Manchester | `manchester` | 4.00, 3.00, 2.00 | 24,000 |
| Man. diferencial | `manchester-dif` | 4.00, 3.00, 2.00 | 24,000 |
| Escurrido | `escurrido` | 2.50, 2.25, 2.00, 1.50, 1.33 | 36,000 |
| Rayo (added by this port) | `rayo` | 2.75, 2.25 | 21,333, before compression |

The last column is the rate at the **fastest** setting each technique offers.
It says nothing about whether that setting loads on real hardware; section 10
does. The default of 2.75 samples/bit gives 17,454 bps.

Bit rate is `sample rate ÷ samples per bit`, except at 1.33, which gives
three-quarters of the sample rate.

### Scheme restrictions

| | All in one block | Many blocks | As the original loader |
|---|---|---|---|
| ROM, Milks, FSK, Shavings Slow, Shavings Delta, Ultra, NPU, Fi, Fi Quadruple, Manchester, Man. diferencial, Escurrido | ✓ | ✓ | — |
| Shavings Raudo | ✓ | — | — |
| Rayo | ✓ | — | — |
| Any snapshot | — (loaded whole) | — | — |

### Checkbox restrictions

| Option | Available when |
|---|---|
| Verify tape loading error | High speed mode, and not ROM / Shavings Delta / Shavings Raudo / Ultra / Fi Quadruple / Escurrido / Rayo |
| Statistical optimisation | High speed mode and Shavings Delta only |
| Compress (Rayo) | High speed mode and Rayo only |
| Reverse right channel | Stereo is on |
| Encoder | LAME mp3 encoder is on |
| ->Emu | High speed mode and a file is loaded |

### Defaults

| Setting | Default |
|---|---|
| Conversion mode | High speed |
| Technique | Shavings Raudo |
| Samples per bit | 2.75 |
| Sampling frequency | 48000 Hz |
| Waveform | Cubic |
| Loading scheme | Many blocks |
| Generate the BASIC loader | on |
| Verify tape loading error | on |
| LAME mp3 encoder | off, `lame`, 320 kbps fixed |
| Info in output file name | off |

---

## Credits

k7zx 4.3 was written by Francisco Villa. This is a port of
their work to modern C++ on Linux; the conversion algorithms, the hand-assembled
Z80 loader routines and the interface layout are theirs. The original
Borland C++Builder 6 sources are preserved in `original/`. The Rayo technique,
its compressor and the reliability tests in sections 6 and 10 were added by
the port.
