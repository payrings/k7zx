// k7zx 5.0 - modern C++ port
//
// Constants shared across the engine.  The names are kept identical to the
// originals in the Borland C++Builder sources (zxwav.h, zxcode.h, zxfiles.h,
// loaderRoutines.h) so that the ported algorithms can be diffed line by line against
// the 1998-era code.
#ifndef K7ZX_DEFS_H
#define K7ZX_DEFS_H

#include <cstdint>

namespace k7zx {

// ---------------------------------------------------------------------------
// Loading "methods" -- the tape encoding techniques.  Values are meaningful:
// they index the code tables in loader_data.cpp and the explanatory texts.
//
// 13-18 were the low-rate "Ma non troppo" techniques (Andante, Allegro
// Agitato, Vivace, Presto, Allegro Maestoso, Allegro Scherzando) and 19 was
// Veloz; both were removed in step 15.  The values are reserved, not reused:
// the settings file stores the technique as an integer under `Ops.Method`
// (legacy `Ops.MetodoHI`), so renumbering would silently switch users' saved
// choices.  See README step 15.
// ---------------------------------------------------------------------------
enum Method : int {
    kRom = 0,
    kMilks = 1,
    kFsk = 2,
    kShavingsSlow = 3,
    kShavingsDelta = 4,
    kShavingsRaudo = 5,
    kUltra = 6,
    kNpu = 7,
    kFi = 8,
    kFiQ = 9,
    kManchester = 10,
    kManchesterDif = 11,
    kEscurrido = 12,
    // 13-18: low-rate techniques, removed.  19: Veloz, removed.
    // Added by this port: Raudo's signal with a checksum, polarity detection,
    // per-tape symbol mapping and optional LZ compression (rayo.h).
    kRayo = 20,
    kMethodCount = 21
};

// ---------------------------------------------------------------------------
// Samples-per-bit selectors.  The numeric value is the "samples per bit"
// quarter-units used by the UI (bps = 4 * sampleRate / mpb), exactly as in the
// original.  mpb == 4 is the special 1.33 bps (3 samples per 2 bits) case.
// ---------------------------------------------------------------------------
enum SamplesPerBit : int {
    kS8_00 = 32,
    kS7_00 = 28,
    kS6_00 = 24,
    kS5_00 = 20,
    kS4_00 = 16,
    kS3_50 = 14,
    kS3_00 = 12,
    kS2_75 = 11,
    kS2_50 = 10,
    kS2_25 = 9,
    kS2_00 = 8,
    kS1_75 = 7,
    kS1_50 = 6,
    kS1_25 = 5,
    kS1_33 = 4
};

// ---------------------------------------------------------------------------
// Waveform shapes.
// ---------------------------------------------------------------------------
enum Waveform : int {
    kSquare = 0,
    kRamp = 1,
    kCubic = 2,
    kZeroContinuous = 3,
    kEqualEnergy = 4,
    kParabola = 5,
    kDelta = 6
};

// ---------------------------------------------------------------------------
// Loading schemes (how the blocks are fed to the Spectrum).
// ---------------------------------------------------------------------------
enum Scheme : int {
    kSnapshot = 0,
    kOneBlock = 1,
    kManyBlocks = 2,
    kOriginalLoader = 3
};

// ---------------------------------------------------------------------------
// Top level conversion modes offered by the original UI.
//   0 = "normal"  : faithful tap/tzx -> wav (like tap2wav / tzx2wav)
//   1 = "hispeed" : turbo loader -> wav, loads in seconds
// 2 = the low-rate "Ma non troppo" mode, removed in step 15.  The value is
//     reserved, not reused: the settings file stores it under `Ops.Mode`
//     (legacy `Ops.Conversor`).  See README step 15.
// ---------------------------------------------------------------------------
enum ConversionMode : int {
    kConvertNormal = 0,
    kConvertHiSpeed = 1,
    kConvertModeCount = 3
};

// ZX Spectrum ULA tick rate.  Every pulse length in the code tables is
// expressed in T-states.
constexpr double kUlaHz = 3500000.0;

// Used by the "normal" TAP->WAV renderer to convert T-states to samples.
constexpr double kTsPerSample = 79.36508;  // _79_ in the original

// Highest address a tape block may touch before it clobbers the system
// variables.  Original: _MAX_ADDRESS_.
constexpr unsigned kMaxAddress = 0xff3c;

// Well-known addresses used when building the synthetic loader program.
constexpr unsigned kVarsAddress = 23552;  // the system variables block
constexpr unsigned kBasicProgram = 23755;  // where a reloaded BASIC body lands
}  // namespace k7zx

#endif  // K7ZX_DEFS_H
