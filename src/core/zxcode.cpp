// k7zx 5.0 - modern C++ port
//
// The loader synthesizer, ported from ZXCODE.CPP.
#include "zxcode.h"

#include <algorithm>
#include <initializer_list>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "byteorder.h"
#include "rayo.h"

namespace k7zx {
namespace {

/// Z80 opcodes used when re-timing the Manchester loaders.
constexpr std::uint8_t kIncHl = 0x34;
constexpr std::uint8_t kDecHl = 0x35;
constexpr std::uint8_t kExSpHl = 0xe3;

/// Address of the first byte of the loader inside the synthetic BASIC line.
constexpr unsigned kLoaderBase = 23755;
/// The original spelled this `23797 - 38`; kept as a named constant because
/// it shows up in three different places.
constexpr unsigned kLoaderBaseAdjust = 23797 - 38;

}  // namespace

// ===========================================================================
// HighSpeedConverter
// ===========================================================================
HighSpeedConverter::HighSpeedConverter(TapeImage& image, const AudioOptions& audio,
                                       const LoaderOptions& loader)
    : image_(image), audioOpts_(audio), loaderOpts_(loader) {}

// ---------------------------------------------------------------------------
// Routine patching helpers
// ---------------------------------------------------------------------------
void HighSpeedConverter::copyRoutine(Method m) {
    std::memset(loaderRoutine_.data(), 0, loaderRoutine_.size());
    const std::size_t n = std::min(CodeSizes::primary(m), loaderRoutine_.size());
    std::memcpy(loaderRoutine_.data(), data::codes[m], n);
    maxPos_ = loaderRoutine_[0] + 1;
    eiPos_ = loaderRoutine_[3];
    jpPos_ = eiPos_ + 1;
    routineLength_ = 0;
    while (5 + static_cast<std::size_t>(routineLength_) < loaderRoutine_.size() && loaderRoutine_[5 + routineLength_])
        ++routineLength_;
}

void HighSpeedConverter::copyExtraRoutine(ExtraRoutine r) {
    std::memset(loaderRoutine_.data(), 0, loaderRoutine_.size());
    const std::size_t n = std::min(CodeSizes::extra(r), loaderRoutine_.size());
    std::memcpy(loaderRoutine_.data(), data::codes_extra[r], n);
    maxPos_ = loaderRoutine_[0] + 1;
    eiPos_ = loaderRoutine_[3];
    jpPos_ = eiPos_ + 1;
    routineLength_ = 0;
    while (5 + static_cast<std::size_t>(routineLength_) < loaderRoutine_.size() && loaderRoutine_[5 + routineLength_])
        ++routineLength_;
}

/// The four Shavings-Delta pulse widths are chosen by counting how often each
/// of the four possible 2-bit groups appears in the payload, so the most
/// common group gets the shortest pulse.  `antiKolmogorov` enables the count;
/// without it the groups keep their natural order 0,1,2,3.
void HighSpeedConverter::statisticalAnalysis() {
    for (int t = 0; t < 4; ++t) {
        histo_[static_cast<std::size_t>(t)].count = 0;
        histo_[static_cast<std::size_t>(t)].symbol = t;
    }
    if (loaderOpts_.antiKolmogorov) {
        for (unsigned i = newStart_; i < newEnd_; ++i) {
            const unsigned char b = image_.memory()[i];
            histo_[b >> 6].count += 1;
            histo_[(b >> 4) & 3].count += 1;
            histo_[(b >> 2) & 3].count += 1;
            histo_[b & 3].count += 1;
        }
        for (int t = 0; t < 3; ++t)
            for (int i = t + 1; i < 4; ++i)
                if (histo_[static_cast<std::size_t>(t)].count < histo_[static_cast<std::size_t>(i)].count)
                    std::swap(histo_[static_cast<std::size_t>(t)], histo_[static_cast<std::size_t>(i)]);
    }
    deltaDurations_[histo_[0].symbol] = 1;
    deltaDurations_[histo_[1].symbol] = 2;
    deltaDurations_[histo_[2].symbol] = 3;
    deltaDurations_[histo_[3].symbol] = 4;
}

// --- Table builders ---------------------------------------------------------
// Each writes a run of threshold values into the patched loader at offset x.
// `d00..d11` are pulse counts at which the symbol changes.

// The original loops of make_tabla and make_tabla_slow are `int i=1; while (i++)`
// -- the post-increment means the first index used is 2, not 1.  Starting at 1
// shifts the Slow table one byte early (clobbering the opcode in front of it)
// and makes the Delta table write one extra byte.
void HighSpeedConverter::makeTable(int x, int d00, int d01, int d10, int d11) {
    for (int i = 2;; ++i) {
        const int idx = x + (3 * i) / 2;
        if (idx < 0 || static_cast<std::size_t>(idx) >= loaderRoutine_.size()) return;
        if (i < d00)
            loaderRoutine_[static_cast<std::size_t>(idx)] = static_cast<std::uint8_t>(0xfc | histo_[0].symbol);
        else if (i < d01)
            loaderRoutine_[static_cast<std::size_t>(idx)] = static_cast<std::uint8_t>(0xfc | histo_[1].symbol);
        else if (i < d10)
            loaderRoutine_[static_cast<std::size_t>(idx)] = static_cast<std::uint8_t>(0xfc | histo_[2].symbol);
        else if (i < d11)
            loaderRoutine_[static_cast<std::size_t>(idx)] = static_cast<std::uint8_t>(0xfc | histo_[3].symbol);
        else
            return;
    }
}

void HighSpeedConverter::makeTableRaudo(int x, int d00, int d01, int d10, int d11) {
    for (int i = 2;; i += 3) {
        const int idx = x + i;
        if (idx < 0 || static_cast<std::size_t>(idx) >= loaderRoutine_.size()) return;
        if (i < d00)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 0;
        else if (i < d01)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 1;
        else if (i < d10)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 2;
        else if (i < d11)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 3;
        else
            return;
    }
}

void HighSpeedConverter::makeTableSlow(int x, int d00, int d01, int d10, int d11) {
    const int base = x - 2;
    for (int i = 2;; ++i) {
        const int idx = base + i;
        if (idx < 0 || static_cast<std::size_t>(idx) >= loaderRoutine_.size()) return;
        if (i < d00)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 0xfc;
        else if (i < d01)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 0xfd;
        else if (i < d10)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 0xfe;
        else if (i < d11)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 0xff;
        else
            return;
    }
}


void HighSpeedConverter::makeTableEscu2(int x, int d01, int d10, int d11, int d00) {
    for (int i = 0;; ++i) {
        const int idx = x + (3 * (i + 1)) / 2;
        if (idx < 0 || static_cast<std::size_t>(idx) >= loaderRoutine_.size()) return;
        if (i + 1 < d01)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 0xfd;
        else if (i + 1 < d10)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 0xfe;
        else if (i + 1 < d11)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 0xff;
        else if (i + 1 < d00)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 0xfc;
        else
            return;
    }
}

void HighSpeedConverter::makeTableEscu1(int x, int d01, int d10, int d11, int d00) {
    for (int i = 0;; ++i) {
        const int idx = x + i + 1;
        if (idx < 0 || static_cast<std::size_t>(idx) >= loaderRoutine_.size()) return;
        if (i + 1 < d01)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 0xfd;
        else if (i + 1 < d10)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 0xfe;
        else if (i + 1 < d11)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 0xff;
        else if (i + 1 < d00)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 0xfc;
        else
            return;
    }
}

void HighSpeedConverter::makeTableNpuP(int x, int d00, int d01, int d10, int d11) {
    for (int i = 0;; ++i) {
        const int idx = x + i + 1;
        if (idx < 0 || static_cast<std::size_t>(idx) >= loaderRoutine_.size()) return;
        if (i + 1 < d00)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 0;
        else if (i + 1 < d01)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 1;
        else if (i + 1 < d10)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 2;
        else if (i + 1 < d11)
            loaderRoutine_[static_cast<std::size_t>(idx)] = 3;
        else
            return;
    }
}

// Original: `int i=0; while (--i)`, so the first index used is -1.

// ---------------------------------------------------------------------------
// prepareLoaderRoutine: per-method timing patches
// ---------------------------------------------------------------------------
void HighSpeedConverter::prepareLoaderRoutine() {
    const Method method = static_cast<Method>(audioOpts_.method);
    const int spb = audioOpts_.samplesPerBit;
    const bool r44 = audioOpts_.sampleRate == 44100;

    copyRoutine(method);

    auto& R = loaderRoutine_;
    auto at = [&](int i) -> std::uint8_t& { return R[static_cast<std::size_t>(i)]; };
    // _POS_K0_ and _POS_K1_ were macros in the original -- `(rutina[1]+1)` and
    // `(rutina[2]+1)` -- evaluated at every use.  Several cases swap in an
    // extra routine with copyExtraRoutine() and then patch it, so the offsets
    // must be read from the routine that is loaded *now*, not the primary one.
    auto k0 = [&]() -> int { return R[1] + 1; };
    auto k1 = [&]() -> int { return R[2] + 1; };

    switch (method) {
        case kRom:
            if (spb == kS2_50) {
                copyExtraRoutine(kRomMini);
                at(k0()) = r44 ? static_cast<std::uint8_t>(0xf2 + 3)
                             : static_cast<std::uint8_t>(0xf2 + 6);
            } else {
                at(k0()) = r44 ? 12 : 9;
                at(k1()) = 0x1f;
            }
            break;

        case kMilks:
            if (r44) {
                if (spb == kS3_50 || spb == kS2_75) R[26] = 0xfc;
            } else {
                R[26] = 0xfc;
                R[30] = 0xfd;
                R[36] = 0xfe;
                R[41] = 0xff;
            }
            break;

        case kFsk:
            if (r44) {
                switch (spb) {
                    case kS8_00: at(R[2] + 1) = 0xda; break;
                    case kS7_00: at(R[2] + 1) = static_cast<std::uint8_t>(0xe9 - 0x10); break;
                    case kS6_00: at(R[2] + 1) = static_cast<std::uint8_t>(0xec - 0x10); break;
                    case kS5_00: at(R[2] + 1) = static_cast<std::uint8_t>(0xf2 - 0x10); break;
                    case kS4_00: at(R[2] + 1) = static_cast<std::uint8_t>(0xf5 - 0x10); break;
                    case kS3_00: at(R[2] + 1) = static_cast<std::uint8_t>(0xf8 - 0x10); break;
                    case kS2_50:
                        copyExtraRoutine(kFskR);
                        at(R[2] + 1) = static_cast<std::uint8_t>(109 - 3 - 3);
                        break;
                    default: break;
                }
            } else {
                switch (spb) {
                    case kS8_00: at(R[2] + 1) = 0xdb; break;
                    case kS7_00: at(R[2] + 1) = static_cast<std::uint8_t>(0xec - 0x10); break;
                    case kS6_00: at(R[2] + 1) = static_cast<std::uint8_t>(0xf0 - 0x10); break;
                    case kS5_00: at(R[2] + 1) = static_cast<std::uint8_t>(0xf3 - 0x10); break;
                    case kS4_00: at(R[2] + 1) = static_cast<std::uint8_t>(0xf6 - 0x10); break;
                    case kS3_00: at(R[2] + 1) = static_cast<std::uint8_t>(0xf9 - 0x10); break;
                    case kS2_50:
                        copyExtraRoutine(kFskR);
                        at(R[2] + 1) = static_cast<std::uint8_t>(107 - 3);
                        break;
                    default: break;
                }
            }
            break;

        case kShavingsSlow:
            if (r44) {
                switch (spb) {
                    case kS1_75: at(k0()) = static_cast<std::uint8_t>(at(k0()) - 2);
                                makeTableSlow(R[2], 5, 7, 10, 14); break;
                    case kS2_50: at(k0()) = static_cast<std::uint8_t>(at(k0()) - 2);
                                makeTableSlow(R[2], 7, 11, 17, 24); break;
                    case kS3_00: at(k0()) = static_cast<std::uint8_t>(at(k0()) - 5);
                                makeTableSlow(R[2], 7, 12, 17, 23); break;
                    case kS3_50: at(k0()) = static_cast<std::uint8_t>(at(k0()) - 7);
                                makeTableSlow(R[2], 7, 12, 17, 23); break;
                    case kS4_00: at(k0()) = static_cast<std::uint8_t>(at(k0()) - 10);
                                makeTableSlow(R[2], 7, 12, 17, 23); break;
                    default: break;
                }
            } else {
                switch (spb) {
                    case kS1_75:
                        at(k0()) = static_cast<std::uint8_t>(at(k0()) - 2);
                        at(k0() + 3) = at(k0() - 10) = 0x2c;
                        makeTableSlow(R[2], 4, 7, 10, 14);
                        break;
                    case kS2_50:
                        at(k0() + 3) = at(k0() - 10) = 0x2c;
                        at(k0()) = static_cast<std::uint8_t>(at(k0()) - 2);
                        makeTableSlow(R[2], 6, 11, 16, 22);
                        break;
                    case kS3_00:
                        at(k0()) = static_cast<std::uint8_t>(at(k0()) - 4);
                        makeTableSlow(R[2], 6, 11, 16, 22);
                        break;
                    case kS3_50:
                        at(k0()) = static_cast<std::uint8_t>(at(k0()) - 7);
                        makeTableSlow(R[2], 6, 11, 15, 21);
                        break;
                    case kS4_00:
                        at(k0()) = static_cast<std::uint8_t>(at(k0()) - 9);
                        makeTableSlow(R[2], 6, 11, 16, 22);
                        break;
                    default: break;
                }
            }
            break;

        case kShavingsDelta:
            statisticalAnalysis();
            if (r44) {
                switch (spb) {
                    case kS1_75: makeTable(R[1] - 3, 6, 10, 13, 26); break;
                    case kS2_25: makeTable(R[1] - 3, 9, 13, 16, 26); break;
                    case kS2_75: makeTable(R[1] - 3, 13, 16, 20, 26); break;
                    case kS3_00: makeTable(R[1] - 3, 10, 17, 24, 36); break;
                    case kS3_50: makeTable(R[1] - 3, 14, 21, 28, 37); break;
                    default: break;
                }
            } else {
                switch (spb) {
                    case kS1_75: makeTable(R[1] - 3, 5, 8, 12, 25); break;
                    case kS2_25: makeTable(R[1] - 3, 8, 12, 15, 25); break;
                    case kS2_75: makeTable(R[1] - 3, 12, 15, 18, 25); break;
                    case kS3_00: makeTable(R[1] - 3, 10, 16, 23, 35); break;
                    case kS3_50: makeTable(R[1] - 3, 13, 19, 25, 34); break;
                    default: break;
                }
            }
            break;

        case kShavingsRaudo:
            routineLength_ = 277;
            if (r44) {
                switch (spb) {
                    case kS1_75:
                        makeTableRaudo(R[2], 18, 33, 48 - 3, 71);
                        makeTableRaudo(R[2], 18, 33, 48 - 3, 71);
                        break;
                    case kS2_25: makeTableRaudo(R[2], 36 - 3, 51 - 3, 66 - 3, 85); break;
                    case kS2_50: makeTableRaudo(R[2], 24, 54, 84, 108); break;
                    case kS2_75: makeTableRaudo(R[2], 51 - 3, 66 - 3, 81 - 3, 99); break;
                    default: break;
                }
            } else {
                switch (spb) {
                    case kS1_75:
                        at(R[1]) = at(R[1] + 0x40) = 0x3e;
                        makeTableRaudo(R[2], 15, 27, 39 + 3, 84);
                        copyExtraRoutine(kRaudo48000);
                        routineLength_ = 262;
                        makeTableRaudo(R[2] + 1, 20, 30, 42 + 3, 68);
                        break;
                    case kS2_25: makeTableRaudo(R[2], 30, 42, 54, 78); break;
                    case kS2_50: makeTableRaudo(R[2], 21, 45, 72, 99); break;
                    case kS2_75: makeTableRaudo(R[2], 42 + 3, 54 + 3, 66 + 3, 84); break;
                    default: break;
                }
            }
            break;

        case kUltra:
            if (r44) {
                switch (spb) {
                    case kS1_50:
                        copyExtraRoutine(kUltraR);
                        at(k0()) = 12;
                        at(k1()) = static_cast<std::uint8_t>(212);
                        break;
                    case kS2_00: at(k0()) = static_cast<std::uint8_t>((0x78 + 0x7e) / 2); break;
                    case kS2_50:
                        copyExtraRoutine(kUltraR);
                        at(k0()) = static_cast<std::uint8_t>(12 - 9);
                        at(k1()) = static_cast<std::uint8_t>(212 - 9);
                        break;
                    case kS3_00: at(k0()) = 0x78; break;
                    default: break;
                }
            } else {
                switch (spb) {
                    case kS1_50:
                        copyExtraRoutine(kUltraR);
                        at(k0()) = 13;
                        at(k1()) = static_cast<std::uint8_t>(213);
                        break;
                    case kS2_00: at(k0()) = 0x7b; break;
                    case kS2_50:
                        copyExtraRoutine(kUltraR);
                        at(k0()) = static_cast<std::uint8_t>(2 + 13 - 9);
                        at(k1()) = static_cast<std::uint8_t>(2 + 213 - 12);
                        break;
                    case kS3_00: at(k0()) = 0x79; break;
                    default: break;
                }
            }
            break;

        case kNpu:
            switch (spb) {
                case kS1_25:
                    copyExtraRoutine(kNpuR);
                    routineLength_ = 16 + R[jpPos_ + 1] - R[19];
                    if (r44) makeTableNpuP(R[2], 11, 23, 32, 55);
                    else makeTableNpuP(R[2], 11, 21, 31, 55);
                    break;
                case kS1_75:
                    copyExtraRoutine(kNpuR);
                    routineLength_ = 16 + R[jpPos_ + 1] - R[19];
                    if (r44) makeTableNpuP(R[2], 23, 35, 44, 58);
                    else makeTableNpuP(R[2], 22, 31, 43, 55);
                    break;
                case kS2_00:
                    routineLength_ = 16 + R[jpPos_ + 1] - R[19];
                    if (r44) {
                        makeTableNpuP(R[2] - 1, 5, 10, 15, 23);
                    } else {
                        at(R[1]) = at(R[1] + 14) = at(R[1] + 28) = at(R[1] + 42) = 0x2c;
                        makeTableNpuP(R[2] - 1, 5, 10, 15, 23);
                    }
                    break;
                case kS2_50:
                    routineLength_ = 16 + R[jpPos_ + 1] - R[19];
                    if (r44) {
                        makeTableNpuP(R[2] - 1, 8, 13, 18, 25);
                    } else {
                        at(R[1]) = at(R[1] + 14) = at(R[1] + 28) = at(R[1] + 42) = 0x2c;
                        makeTableNpuP(R[2] - 1, 8, 12, 17, 24);
                    }
                    break;
                default: break;
            }
            break;

        case kFi:
            // DELIBERATE DIVERGENCE FROM k7zx 4.3, at this one speed only: the
            // WAVs differ, and 4.3's does not load (tools/zxload leaves its CPU
            // running) while this one halts at $8002.  Fi at 8.00..4.00 is
            // byte-identical to 4.3's.
            //
            // The cause is not established.  A comment here used to claim 4.3's
            // `else /* ... */ break;` fell through into case _FI_Q_; it does
            // not -- the comment closes before the break, so the break is the
            // else arm and this matches 4.3 statement for statement.  Nor is
            // it 4.3's fixed 360-byte copiarutina() reading past the 122-byte
            // fi_mini: zeroing what it pulls in does not change its output.
            if (spb == kS3_00) {
                copyExtraRoutine(kFiMini);
                at(k0()) = static_cast<std::uint8_t>(8 - 2);
            }
            break;

        case kFiQ:
            if (r44) {
                switch (spb) {
                    case kS1_75:
                        copyExtraRoutine(kFiQFast);
                        at(k1()) = 0x2b;
                        at(k0()) = 0x8;
                        break;
                    case kS2_00: at(k1()) = static_cast<std::uint8_t>(0x55 + 3);
                                at(k0()) = static_cast<std::uint8_t>(at(k1()) + 0x88 + 3); break;
                    case kS2_25: at(k1()) = 0x55;
                                at(k0()) = static_cast<std::uint8_t>(at(k1()) + 0x88 + 3); break;
                    case kS2_50: at(k1()) = 0x4f;
                                at(k0()) = static_cast<std::uint8_t>(at(k1()) + 0x88 + 3); break;
                    case kS2_75: at(k1()) = 0x4a;
                                at(k0()) = static_cast<std::uint8_t>(at(k1()) + 0x88 + 3); break;
                    case kS3_00: at(k1()) = 0x41;
                                at(k0()) = static_cast<std::uint8_t>(at(k1()) + 0x88 + 3); break;
                    case kS3_50: at(k1()) = 0x37;
                                at(k0()) = static_cast<std::uint8_t>(at(k1()) + 0x88 + 3); break;
                    default: break;
                }
            } else {
                switch (spb) {
                    case kS1_75: copyExtraRoutine(kFiQFast); at(k1()) = 0x28; break;
                    case kS2_00: at(k1()) = 0x59;
                                at(k0()) = static_cast<std::uint8_t>(at(k1()) + 0x88 + 3); break;
                    case kS2_25: at(k1()) = 0x59;
                                at(k0()) = static_cast<std::uint8_t>(at(k1()) + 0x88 + 3); break;
                    case kS2_50: at(k1()) = 0x50;
                                at(k0()) = static_cast<std::uint8_t>(at(k1()) + 0x88); break;
                    case kS2_75: at(k1()) = 0x50;
                                at(k0()) = static_cast<std::uint8_t>(at(k1()) + 0x88); break;
                    case kS3_00: at(k1()) = 0x47;
                                at(k0()) = static_cast<std::uint8_t>(at(k1()) + 0x88); break;
                    case kS3_50: at(k1()) = 0x3d;
                                at(k0()) = static_cast<std::uint8_t>(at(k1()) + 0x88 + 3); break;
                    default: break;
                }
            }
            break;

        case kManchester:
            at(R[2] + 1) = at(R[1] + 1);
            switch (spb) {
                case kS2_00:
                    at(R[2] + 1) = static_cast<std::uint8_t>(at(R[2] + 1) - (r44 ? 4 : 2));
                    at(R[1] + 1) = static_cast<std::uint8_t>(at(R[1] + 1) - (r44 ? 4 : 2));
                    break;
                case kS3_00:
                    at(R[2] + 1) = static_cast<std::uint8_t>(at(R[2] + 1) - (r44 ? 4 : 5));
                    at(R[1] + 1) = static_cast<std::uint8_t>(at(R[1] + 1) - (r44 ? 4 : 5));
                    if (r44) {
                        at(R[2] + 7) = kIncHl;  at(R[2] + 8) = kDecHl;
                        at(R[2] + 9) = kIncHl;  at(R[2] + 10) = kDecHl;
                    } else {
                        at(R[2] + 9) = kDecHl;  at(R[2] + 10) = kIncHl;
                    }
                    break;
                case kS4_00:
                    at(R[2] + 1) = static_cast<std::uint8_t>(at(R[2] + 1) - 8);
                    at(R[1] + 1) = static_cast<std::uint8_t>(at(R[1] + 1) - 8);
                    if (r44) {
                        at(R[2] + 3) = kExSpHl; at(R[2] + 4) = kExSpHl;
                        at(R[2] + 5) = kIncHl;  at(R[2] + 6) = kDecHl;
                        at(R[2] + 7) = kIncHl;  at(R[2] + 8) = kDecHl;
                        at(R[2] + 9) = kIncHl;  at(R[2] + 10) = kDecHl;
                    } else {
                        at(R[2] + 3) = kIncHl;  at(R[2] + 4) = kDecHl;
                        at(R[2] + 5) = kIncHl;  at(R[2] + 6) = kDecHl;
                        at(R[2] + 7) = kIncHl;  at(R[2] + 8) = kDecHl;
                        at(R[2] + 9) = kIncHl;  at(R[2] + 10) = kDecHl;
                    }
                    break;
                default: break;
            }
            break;

        case kManchesterDif:
            at(R[2] + 1) = at(R[1] + 1);
            switch (spb) {
                case kS2_00:
                    at(R[1] + 2) = static_cast<std::uint8_t>(at(R[1] + 2) - (r44 ? 3 : 1));
                    at(R[2] + 2) = static_cast<std::uint8_t>(at(R[2] + 2) - (r44 ? 3 : 1));
                    break;
                case kS3_00:
                    at(R[1] + 2) = static_cast<std::uint8_t>(at(R[1] + 2) - 4);
                    at(R[2] + 2) = static_cast<std::uint8_t>(at(R[2] + 2) - 4);
                    if (r44) {
                        at(R[1] - 12) = kIncHl; at(R[1] - 13) = kDecHl;
                        at(R[1] - 14) = kIncHl; at(R[1] - 15) = kDecHl;
                        at(R[2] - 12) = kIncHl; at(R[2] - 13) = kDecHl;
                        at(R[2] - 14) = kIncHl; at(R[2] - 15) = kDecHl;
                    } else {
                        at(R[1] - 12) = kIncHl; at(R[1] - 13) = kDecHl;
                        at(R[2] - 12) = kIncHl; at(R[2] - 13) = kDecHl;
                    }
                    break;
                case kS4_00: {
                    // Six filler bytes before each sampling point, exactly as
                    // the original: EX (SP),HL and INC/DEC HL come in pairs so
                    // that HL is unchanged -- 44.1 kHz: EX EX EX EX INC DEC;
                    // 48 kHz: EX EX INC DEC INC DEC.
                    static const std::uint8_t fill44[6] = {kExSpHl, kExSpHl, kExSpHl,
                                                           kExSpHl, kIncHl,  kDecHl};
                    static const std::uint8_t fill48[6] = {kExSpHl, kExSpHl, kIncHl,
                                                           kDecHl,  kIncHl,  kDecHl};
                    const std::uint8_t* fill = r44 ? fill44 : fill48;
                    at(R[1] + 2) = static_cast<std::uint8_t>(at(R[1] + 2) - 6);
                    at(R[2] + 2) = static_cast<std::uint8_t>(at(R[2] + 2) - 6);
                    for (int k = 0; k < 6; ++k) {
                        at(R[1] - 12 - k) = fill[k];
                        at(R[2] - 12 - k) = fill[k];
                    }
                    break;
                }
                default: break;
            }
            break;

        case kEscurrido:
            if (r44) {
                switch (spb) {
                    case kS1_33:
                        makeTableEscu2(R[1] - 1, 3, 7, 10, 14);
                        at(R[2] + 16) = 0xed;
                        at(R[2] + 17) = 0x41;
                        break;
                    case kS1_50: makeTableEscu2(R[1] - 1, 3, 7, 10, 14); break;
                    case kS2_00: makeTableEscu2(R[1] - 1, 7, 10, 14, 18); break;
                    case kS2_25:
                        copyExtraRoutine(kEscSlow);
                        at(R[2] + 12) = 8;
                        makeTableEscu1(R[1], 8, 13, 19, 25);
                        at(R[2] + 1) = static_cast<std::uint8_t>(at(R[2] + 1) + 7);
                        break;
                    case kS2_50:
                        copyExtraRoutine(kEscSlow);
                        at(R[2] + 12) = 10;
                        makeTableEscu1(R[1], 8, 13, 18, 25);
                        at(R[2] - 18) = 5;
                        break;
                    default: break;
                }
            } else {
                switch (spb) {
                    case kS1_33:
                        makeTableEscu2(R[1] - 1, 3, 7, 10, 14);
                        at(R[2] + 1) = static_cast<std::uint8_t>(at(R[2] + 1) + 2);
                        // Original: `rutina[..+16]=0x40;0xed; rutina[..+17]=0x40;0x41;`
                        // -- the second value on each line is a dead expression
                        // statement, so both bytes are 0x40 (LD B,B, a 4 T no-op
                        // replacing the 44.1 kHz OUT (C),B).
                        at(R[2] + 16) = 0x40;
                        at(R[2] + 17) = 0x40;
                        break;
                    case kS1_50:
                        makeTableEscu2(R[1] - 1, 3, 7, 10, 14);
                        at(R[2] + 1) = static_cast<std::uint8_t>(at(R[2] + 1) + 2);
                        at(R[2] + 17) = static_cast<std::uint8_t>(at(R[2] + 17) + 2);
                        break;
                    case kS2_00:
                        makeTableEscu2(R[1] - 1, 7, 10, 14, 18);
                        at(R[2] + 17) = static_cast<std::uint8_t>(at(R[2] + 17) + 2);
                        at(R[2] + 1) = static_cast<std::uint8_t>(at(R[2] + 1) + 2);
                        break;
                    case kS2_25:
                        copyExtraRoutine(kEscSlow);
                        makeTableEscu1(R[1], 8, 13, 18, 25);
                        at(R[2] + 12) = 6;
                        at(R[2] + 1) = static_cast<std::uint8_t>(at(R[2] + 1) + 9);
                        break;
                    case kS2_50:
                        copyExtraRoutine(kEscSlow);
                        makeTableEscu1(R[1], 7, 11, 17, 26);
                        at(R[2] + 12) = 9;
                        at(R[2] - 18) = 4;
                        break;
                    default: break;
                }
            }
            break;

        default:
            break;
    }
  }

// ---------------------------------------------------------------------------
// Memory assembly
// ---------------------------------------------------------------------------
/// Point the routine's preamble jump at the multi-block dispatcher.
///
/// The preamble does `ld hl,23797 / ld de,<copy> / ldir / ld hl,n / jp <entry>`.
/// In the multi-block schemes that jump must go to the dispatcher appended
/// after the routine body instead, which then calls the loader once per block.
/// k7zx 4.3 did this with `data[22+14] = data[pos_JP+1]; data[22+15] = 0xff;`
/// -- the low byte of the routine's closing `jp randomizeusr`, which is the
/// address just past the body.  That is only right for loaders relocated to
/// page 0xFF, and it silently breaks if the body length is overridden; so the
/// target is computed from where the dispatcher really lands in the copy.
/// For every technique that k7zx 4.3 allowed in these schemes it produces the
/// same bytes.  Shavings Raudo relocates to 0xFDBF and has no entry point a
/// dispatcher can call, which is why the original never let it use them; see
/// Converter::effectiveScheme().
///
/// The dispatcher's own CALL targets come from the tables in loader_data.cpp.
void HighSpeedConverter::pointPreambleAtDispatcher(std::uint8_t* data) {
    if (static_cast<Method>(audioOpts_.method) == kShavingsRaudo)
        throw std::logic_error("Shavings Raudo cannot be used with a multi-block scheme");
    const unsigned copyDest =
        static_cast<unsigned>(data[22 + 4]) | (static_cast<unsigned>(data[22 + 5]) << 8);
    const unsigned dispatcher = copyDest + routineLength_ - 16;
    data[22 + 14] = static_cast<std::uint8_t>(dispatcher & 0xff);
    data[22 + 15] = static_cast<std::uint8_t>(dispatcher >> 8);
}

void HighSpeedConverter::applyPokes() { image_.applyPokes(); }

void HighSpeedConverter::prepareBasicBlock(int index) {
    const BlockInfo& z = image_.blocks()[static_cast<std::size_t>(index)];
    std::uint8_t* bas = image_.bas().data();
    poke16(bas + 4, 0, static_cast<std::uint16_t>(kLoaderBase + z.param2));
    poke16(bas + 10, 0, static_cast<std::uint16_t>(kLoaderBase + z.length + 1));
    poke16(bas + 38, 0, z.auto_run);

    std::uint8_t* mem = image_.memory().data();
    // The stub is written immediately after the program, so the program has to
    // leave room for it: clamping only against the end of memory still put the
    // stub past it.
    const std::size_t room = TapeImage::kMemorySize - kLoaderBase - image_.bas().size();
    const std::size_t copyLen = std::min<std::size_t>(z.length, room);
    if (copyLen < z.length)
        image_.addWarning("BASIC program does not fit above the loader; truncated\r\n");
    std::memcpy(mem + kLoaderBase, z.data, copyLen);
    // The original copied 60 bytes from a 52 byte table.
    // Offset by what was actually copied, not by the length the header claimed:
    // z.length can now be smaller than that claim (it is capped to the data
    // block), and mem is only 64 KiB, so the claimed length could point far
    // outside it.
    std::memcpy(mem + kLoaderBase + copyLen, bas, image_.bas().size());
}

/// Concatenate every selected block into one contiguous image and compute the
/// highest address it touches.  Returns the number of blocks used.
// k7zx 4.3 terminated its block list at index 7 (`trozos[7].tipo = -1`,
// original/zxfiles.cpp:770) and warned above that, even though the array held
// 32 entries.  The reader here still *finds* up to 31 blocks, so `-v` and the
// GUI can list them all, but only seven can be converted -- and every loop that
// consumes a block list must use the same bound, or the dispatch table the
// loader reads describes blocks whose audio is never sent.
constexpr int kMaxConvertBlocks = 7;

unsigned HighSpeedConverter::joinBlocks() {
    std::uint8_t* mem = image_.memory().data();
    std::memset(mem, 0, TapeImage::kMemorySize);
    std::memcpy(mem + kVarsAddress, data::SYSTEM_VARS, CodeSizes::SYSTEM_VARS);

    totalBlocks_ = 0;
    newStart_ = 0xffff;
    newEnd_ = 0x0000;

    for (int t = 0; t < kMaxConvertBlocks; ++t) {
        const BlockInfo& z = image_.blocks()[static_cast<std::size_t>(t)];
        if (z.isTerminator()) break;
        if (!z.selected || z.data == nullptr) continue;

        if (static_cast<unsigned>(z.startAddress) < newStart_) newStart_ = z.startAddress;
        if (static_cast<unsigned>(z.startAddress) + z.length > newEnd_)
            newEnd_ = static_cast<unsigned>(z.startAddress) + z.length;

        if (loaderOpts_.usrAddress != 7997 && z.execAddress) loaderOpts_.usrAddress = z.execAddress;

        if (z.type == '0') {
            prepareBasicBlock(t);
            newEnd_ += 60;
        } else {
            // The original copied every selected CODE block into the image
            // unconditionally.  The bound here must be the end of memory: an
            // earlier version of this port used kLoaderBase (23755), which
            // silently dropped every block that loads above the BASIC area --
            // i.e. nearly all game code -- so a single-block tape carried zeros.
            if (static_cast<std::size_t>(z.startAddress) + z.length <= TapeImage::kMemorySize)
                std::memcpy(mem + z.startAddress, z.data, z.length);
        }
        ++totalBlocks_;
    }

    // The join below reads at most ten blocks and the multi-block send at most
    // seven, but a tape may carry up to 31.  Anything beyond that used to be
    // dropped with no message at all, so a tape carrying 15 blocks produced a
    // byte-identical file to the same tape with 9.  Say so instead.
    {
        int selected = 0;
        for (const BlockInfo& b : image_.blocks()) {
            if (b.isTerminator()) break;
            if (b.selected) ++selected;
        }
        if (selected > kMaxConvertBlocks) {
            char msg[128];
            std::snprintf(msg, sizeof msg,
                          "%d blocks are selected but only %d fit in a multi-block image;\r\n"
                          "the rest were ignored.\r\n",
                          selected, kMaxConvertBlocks);
            image_.addWarning(msg);
        }
    }

    if (newEnd_ > 0xff40) newEnd_ = 0xff40;
    // A block loading above $ff40 leaves newEnd_ below newStart_, so the span
    // below went negative and, cast to unsigned short, asked the encoder for
    // ~65 kB starting near the top of the image.
    if (newEnd_ < newStart_) newEnd_ = newStart_;
    max_ = 1 + ((newEnd_ - 1) >> 8);
    if (max_ > 0xff) max_ = 0xff;
    return totalBlocks_;
}

void HighSpeedConverter::buildMultiblock() {
    totalBlocks_ = 0;
    const Method method = static_cast<Method>(audioOpts_.method);

    // This loop reads at most ten blocks and the send loop in convertMultiBlock
    // at most seven, while a tape may carry up to 31.  Anything beyond was
    // dropped with no message, so a 15-block tape produced a byte-identical file
    // to the same tape with nine.  Say which blocks went missing.
    {
        int selected = 0;
        for (const BlockInfo& b : image_.blocks()) {
            if (b.isTerminator()) break;
            if (b.selected) ++selected;
        }
        if (selected > kMaxConvertBlocks) {
            char msg[160];
            std::snprintf(msg, sizeof msg,
                          "%d blocks are selected but k7zx 4.3 converted at most %d;\r\n"
                          "the rest were ignored.\r\n",
                          selected, kMaxConvertBlocks);
            image_.addWarning(msg);
        }
    }

    const std::uint8_t* src = data::multi_slow;
    std::size_t srcLen = CodeSizes::multi_slow;
    if (method == kMilks) {
        src = data::multi_milks;
        srcLen = CodeSizes::multi_milks;
    } else if (method == kManchester || method == kManchesterDif) {
        src = data::multi_machester;
        srcLen = CodeSizes::multi_machester;
    }
    std::memset(multiloadCode_.data(), 0, multiloadCode_.size());
    std::memcpy(multiloadCode_.data(), src, std::min(srcLen, multiloadCode_.size()));

    for (int t = 0; t < kMaxConvertBlocks; ++t) {
        BlockInfo& z = image_.blocks()[static_cast<std::size_t>(t)];
        if (z.isTerminator()) break;
        if (!z.selected || z.data == nullptr) continue;

        if (loaderOpts_.usrAddress != 7997 && z.execAddress) loaderOpts_.usrAddress = z.execAddress;

        if (z.type == '0') prepareBasicBlock(t);

        // Keep every block inside the area the loader is allowed to write.  The
        // Manchester branch had no clamp at all, and the clamp the other two
        // used underflowed for a load address already above kMaxAddress:
        // `kMaxAddress - 0xfff0` wraps to a *larger* length, not a smaller one.
        // The Milks and default branches had identical clamps, so one hoisted
        // clamp covers all three.
        if (z.startAddress >= kMaxAddress) {
            z.length = 0;
        } else if (static_cast<unsigned>(z.startAddress) + z.length > kMaxAddress) {
            z.length = static_cast<std::uint16_t>(kMaxAddress - z.startAddress);
        }

        if (method == kManchester || method == kManchesterDif) {
            const std::size_t base = 8 * totalBlocks_;
            if (base + 8 < multiloadCode_.size()) {
                multiloadCode_[2 + base] = static_cast<std::uint8_t>((z.startAddress - 1) & 0xff);
                multiloadCode_[3 + base] = static_cast<std::uint8_t>((z.startAddress - 1) >> 8);
                if (z.type == '0') {
                    multiloadCode_[5 + base] = static_cast<std::uint8_t>(
                        (1 + (0xffu | (static_cast<unsigned>(z.startAddress) + z.length + 60 - 1))) >> 8);
                } else {
                    multiloadCode_[5 + base] = static_cast<std::uint8_t>(
                        (1 + (0xffu | (static_cast<unsigned>(z.startAddress) + z.length - 1))) >> 8);
                }
                if (multiloadCode_[5 + base] == 0) multiloadCode_[5 + base] = 0xff;
            }
        } else if (method != kMilks) {
            // Milks' table already carries the 8-byte dispatch rows it needs
            // (it writes its own $C9 terminator below), so it must not be given
            // the 6-byte rows written here.
            const std::size_t base = 6 * totalBlocks_;
            if (base + 6 < multiloadCode_.size()) {
                multiloadCode_[2 + base] = static_cast<std::uint8_t>((z.startAddress - 1) & 0xff);
                multiloadCode_[3 + base] = static_cast<std::uint8_t>((z.startAddress - 1) >> 8);
            }
        }
        ++totalBlocks_;
    }

    if (method == kMilks) {
        if (4 + totalBlocks_ < multiloadCode_.size()) multiloadCode_[4 + totalBlocks_] = 0xc9;
    } else if (method == kManchester || method == kManchesterDif) {
        if (1 + 8 * totalBlocks_ < multiloadCode_.size()) multiloadCode_[1 + 8 * totalBlocks_] = 0xc9;
    } else {
        if (1 + 6 * totalBlocks_ < multiloadCode_.size()) multiloadCode_[1 + 6 * totalBlocks_] = 0xc9;
    }
}

// ---------------------------------------------------------------------------
// The synthetic BASIC program
// ---------------------------------------------------------------------------
namespace {
constexpr std::size_t kTapHeaderLength = 2;
constexpr std::size_t kTapHeader = 19;  // flag + type + name[10] + 3 words + checksum
constexpr std::size_t kTapFlagff = kTapHeaderLength + kTapHeader;  // 21
constexpr std::size_t kLineOffset = kTapFlagff + 1;                     // 22
constexpr std::size_t kLineHeader = 4;   // line number (2) + length (2)
constexpr std::size_t kLineData = kLineOffset + kLineHeader;            // 26
}  // namespace

std::string HighSpeedConverter::tapeName() const {
    // k7zx 4.3 copied the name bytes verbatim from the source tape's header.
    // The reader trims them for display, so use the raw bytes when the name is
    // still the one from the header; otherwise (a name derived from the file
    // name, or set by the user) pad with spaces as a tape header would be --
    // NUL padding prints as "?" in the ROM's "Program:" line.
    std::string name = image_.prgName().substr(0, 10);
    std::string raw = image_.rawHeaderName();
    std::string rawTrimmed = raw;
    while (!rawTrimmed.empty() && (rawTrimmed.back() == '\0' || rawTrimmed.back() == ' '))
        rawTrimmed.pop_back();
    if (!raw.empty() && rawTrimmed == name) name = raw;
    name.resize(10, ' ');
    return name;
}

void HighSpeedConverter::startBasic() {
    std::memset(basicTap_.data(), 0, basicTap_.size());
    lineOffset_ = kLineData;

    poke16(basicTap_.data(), 0, 19);                        // header_length
    basicTap_[2] = 0x00;                                    // header flag
    basicTap_[3] = 0x00;                                    // header type
    const std::string name = tapeName();
    std::memcpy(basicTap_.data() + 4, name.data(), name.size());
    basicTap_[kTapFlagff] = 0xff;                           // data block flag
    basicTap_[kLineOffset] = 0;                            // line number lo
    basicTap_[kLineOffset + 1] = 0;                        // line number hi

    prepareLoaderRoutine();

    std::uint8_t* data = basicTap_.data() + lineOffset_;
    std::memset(data, 0, 300);

    // CLEAR "n":RANDOMIZE USR "23781":STR$   -- the loader body starts at +22.
    const int n = std::snprintf(reinterpret_cast<char*>(data), 300,
                                "\xfd\xb0\"%u\":\xf9\xc0\xb0\"23781\":\xea", loaderOpts_.clearAddress);
    (void)n;

    std::memcpy(data + 22, loaderRoutine_.data() + 5, static_cast<std::size_t>(routineLength_));
    longLineLength_ = static_cast<unsigned>(22 + routineLength_);

    jpPos_ += 17;
    maxPos_ += 17;
}

void HighSpeedConverter::finishBasic() {
    std::uint8_t* data = basicTap_.data() + lineOffset_;
    data[longLineLength_] = 13;  // CR
    ++longLineLength_;

    // The BASIC line's own length field.  In the original this was
    // `linea_basic->longitud`, a member of the struct that is written to the
    // tape, so it went out as part of the data block.  Leaving it at zero makes
    // the Spectrum's interpreter stop at a zero-length line: the block still
    // loads, but the program is empty and the turbo loader is never entered.
    poke16(basicTap_.data(), kLineOffset + 2, static_cast<std::uint16_t>(longLineLength_));

    const unsigned headerLength = 4 + longLineLength_;
    poke16(basicTap_.data(), 2 + 12, static_cast<std::uint16_t>(headerLength));  // length
    poke16(basicTap_.data(), 2 + 14, 0);   // the header's param 1, unused here
    poke16(basicTap_.data(), 2 + 16, static_cast<std::uint16_t>(headerLength));  // pam2

    // Length of the data block as it would appear in a .tap file.  It is not
    // part of the audio stream -- loaderBlock() is told the length -- and in the
    // original it lived in a `longitub_data` field *after* the 19-byte header
    // block.  Storing it at kTapHeader put it inside that header instead and
    // overwrote param2's high byte, so it is kept here rather than in the block.
    const unsigned dataBlockLength = 2 + headerLength;

    if (loaderOpts_.generateLoader) {
        writer_->loaderBlock(basicTap_.data() + 2, kTapHeader, audioOpts_.accelerateBasic);
        writer_->loaderBlock(basicTap_.data() + kTapFlagff, dataBlockLength,
                             audioOpts_.accelerateBasic);
    }
}

/// Rewrite `LOAD "" CODE` statements into LOAD ""; so the original program's
/// own tape load is suppressed and the turbo loader takes over.
void HighSpeedConverter::changeLoadCode(std::uint8_t* bc, std::size_t size) {
    std::size_t parsed = 0;
    while (parsed < size) {
        if (parsed + 4 > size) break;
        const std::uint16_t lineLen = peek16(bc, parsed + 2);
        if (parsed + 4 + lineLen > size) break;
        std::uint8_t* line = bc + parsed + 4;
        if (lineLen == 0) break;
        if (line[lineLen - 1] != 0x0d) break;

        int position = 1;
        int iniLoad = 0;
        bool hayCode = false;
        bool inQuotes = false;

        for (int c = 0; c < lineLen; ++c) {
            const std::uint8_t ch = line[c];
            if (inQuotes) {
                if (ch == '"') inQuotes = false;
            } else if (ch > 127) {
                switch (ch) {
                    case 0xea: c = lineLen; break;         // STR$
                    case 0xef:                             // LOAD
                        if (position == 1) iniLoad = c + 1;
                        break;
                    case 0xaa:                             // SCREEN$
                    case 0xaf:                             // CODE
                        if (position != 1) hayCode = true;
                        break;
                    default: break;
                }
            } else if (ch == '"') {
                inQuotes = true;
            } else if (ch == ':' || ch == 0x0d) {
                if (iniLoad) {
                    line[iniLoad - 1] = 0xe0;  // LPRINT (see lprint_code)
                    line[iniLoad] = hayCode ? 0x2c : 0x27;  // ',' or '\''
                    line[iniLoad + 1] = ';';
                    int i = iniLoad + 2;
                    while (i < c) line[i++] = ':';
                }
                position = 0;
                iniLoad = 0;
                hayCode = false;
            }
            ++position;
        }
        parsed += 4 + lineLen;
    }
}

/// Patch the loader's relative jumps for 128K snapshot restore.
void HighSpeedConverter::relocate128Code() {
    std::uint8_t* data = basicTap_.data() + lineOffset_;
    std::uint8_t* m128 = image_.multi128().data();
    const Method method = static_cast<Method>(audioOpts_.method);

    // A 0xFF/0xFE/0xFD operand that is really the high byte of a 16-bit
    // address must be moved down by 0x4000: on a 128K the loader runs in the
    // 0x8000-0xBFFF window (0xFF3D -> 0xBF3D).  The two branches of the original used
    // different opcode lists -- Raudo's has LD (nn),HL but no CALL, the
    // generic one has CALL ("4.0 for ultra_r") but no LD (nn),HL -- and they
    // are kept apart here, as is Raudo's third page (0xFD), which carries
    // most of its self-modifying stores.
    const auto precededBy = [&](int i, std::initializer_list<std::uint8_t> ops) {
        if (i >= 1 && data[static_cast<std::size_t>(i - 1)] == 0x26) return true;  // LD H,n
        if (i < 2) return false;
        const std::uint8_t op = data[static_cast<std::size_t>(i - 2)];
        for (std::uint8_t o : ops)
            if (o == op) return true;
        return false;
    };

    if (method == kShavingsRaudo) {
        // Page back to the last one and return.
        data[jpPos_ - 1] = 0x01;  // LD BC,n
        poke16(data + jpPos_, 0, 0x7ffd);
        data[jpPos_ + 2] = 0xc9;  // RET

        data[27] = 0xbd;  // the LDIR destination, 0xFDBF -> 0xBDBF
        for (int i = 22; i < static_cast<int>(longLineLength_); ++i) {
            const std::uint8_t v = data[static_cast<std::size_t>(i)];
            if (v != 0xff && v != 0xfe && v != 0xfd) continue;
            if (precededBy(i, {0x22, 0x32, 0x21, 0x11, 0xc2, 0xc3, 0xea, 0xe2, 0xd2, 0xfa, 0xda}))
                data[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v - 0x40);
        }
    } else {
        data[maxPos_] = 0xbf;  // top of the first page
        data[jpPos_ - 1] = 0x01;
        poke16(data + jpPos_, 0, 0x7ffd);
        data[jpPos_ + 2] = 0xc9;

        for (int i = 22; i < static_cast<int>(longLineLength_); ++i)
            if (data[static_cast<std::size_t>(i)] == 0xff &&
                precededBy(i, {0x32, 0x21, 0x11, 0xc2, 0xc3, 0xea, 0xe2, 0xd2, 0xfa, 0xda, 0xcd}))
                data[static_cast<std::size_t>(i)] = 0xbf;
    }

    // Remember where the loader will live, and point the page-swap helper at
    // the copy of the program it needs to restore.
    poke16(m128 + 74, 0, peek16(data, 22 + 14));
    poke16(data + 22 + 14, 0, static_cast<std::uint16_t>(kLoaderBaseAdjust + longLineLength_));
    poke16(m128 + 1, 0, static_cast<std::uint16_t>(kLoaderBaseAdjust + 22 + longLineLength_));

    if (method != kShavingsRaudo) {
        if (method == kManchester || method == kManchesterDif)
            m128[65] = static_cast<std::uint8_t>(data[22 + 4] + maxPos_ - 20 - 18);
        else
            m128[65] = 6;
    }

    // The original copied 80 bytes out of a 76 byte table; the four trailing
    // bytes stay zero.
    std::memcpy(data + longLineLength_, m128, image_.multi128().size());

    // multi_128 is tuned for loaders that start at 0xFF3D.  The original then
    // shifts nine of its bytes for the two loaders that do not: Raudo (+20)
    // and the fast NPU variant (+194).
    int shift = 0;
    if (method == kShavingsRaudo) shift = 20;
    else if (method == kNpu && audioOpts_.samplesPerBit <= kS1_75) shift = 194;
    if (shift) {
        for (int off : {4, 20, 25, 30, 35, 40, 45, 50, 55})
            data[static_cast<std::size_t>(longLineLength_) + static_cast<std::size_t>(off)] =
                static_cast<std::uint8_t>(data[static_cast<std::size_t>(longLineLength_) +
                                               static_cast<std::size_t>(off)] + shift);
    }

    longLineLength_ += 4 + image_.multi128().size();
}

// ---------------------------------------------------------------------------
// The four conversion shapes
// ---------------------------------------------------------------------------
void HighSpeedConverter::convertSnapshot48K() {
    startBasic();
    std::uint8_t* data = basicTap_.data() + lineOffset_;
    data[maxPos_] = 0xff;
    data[jpPos_ - 1] = 0x040;  // skip the EI so the loader is interruptible
    poke16(data + jpPos_ + 1, 0, static_cast<std::uint16_t>(loaderOpts_.usrAddress));
    finishBasic();

    image_.prepareSnapshotMemoryForConverter(static_cast<Method>(audioOpts_.method) == kShavingsRaudo);
    applyPokes();
    writer_->renderEncodedBlock(image_.blocks()[0].data, 0x4000, image_.blocks()[0].length);
}

void HighSpeedConverter::convertSnapshot128K() {
    startBasic();
    relocate128Code();
    finishBasic();

    image_.prepareSnapshotMemoryForConverter(static_cast<Method>(audioOpts_.method) == kShavingsRaudo);
    applyPokes();

    // The original wrote `t = 0; 0x141;` -- a leftover no-op, so the first
    // block is sent in full.
    const unsigned skip = 0;
    writer_->renderEncodedBlock(image_.blocks()[0].data, 0x4000,
                         static_cast<unsigned short>(image_.blocks()[0].length - skip));

    for (int t = 1; t < 7; ++t) {
        const BlockInfo& z = image_.blocks()[static_cast<std::size_t>(t)];
        if (z.isTerminator()) break;
        if (z.selected) writer_->renderEncodedBlock(z.data, 0xc000, z.length);
    }
}

void HighSpeedConverter::convertSingleBlock() {
    joinBlocks();

    startBasic();
    std::uint8_t* data = basicTap_.data() + lineOffset_;
    data[maxPos_] = static_cast<std::uint8_t>(max_);
    poke16(data + 22 + 11, 0, static_cast<std::uint16_t>(newStart_ - 1));
    if (loaderOpts_.usrAddress == 0)
        data[jpPos_] = 0xc9;  // RET
    else
        data[jpPos_] = 0xc3;  // JP nn
    poke16(data + jpPos_ + 1, 0, static_cast<std::uint16_t>(loaderOpts_.usrAddress));
    finishBasic();

    applyPokes();
    // Belt and braces: the encoder walks `length` bytes from `memory + start`,
    // so the span must fit in the image however it was arrived at.
    unsigned span = newEnd_ - newStart_;
    if (newStart_ >= TapeImage::kMemorySize) span = 0;
    else if (span > TapeImage::kMemorySize - newStart_)
        span = static_cast<unsigned>(TapeImage::kMemorySize) - newStart_;
    writer_->renderEncodedBlock(image_.memory().data() + newStart_, static_cast<unsigned short>(newStart_),
                                static_cast<unsigned short>(span));
}

void HighSpeedConverter::convertMultiBlock() {
    if (!image_.snapshotBlocks()) buildMultiblock();

    startBasic();
    std::uint8_t* data = basicTap_.data() + lineOffset_;
    const Method method = static_cast<Method>(audioOpts_.method);

    poke16(data + 22 + 11, 0,
           static_cast<std::uint16_t>(loaderOpts_.usrAddress ? loaderOpts_.usrAddress : 8019));
    pointPreambleAtDispatcher(data);

    if (loaderOpts_.controlChecksum) {
        data[jpPos_] = 0xC8;  // RET Z
        data[jpPos_ + 1] = 0xCF;  // RST 8
        data[jpPos_ + 2] = 0x1A;  // -> "error in tape"
    } else {
        data[jpPos_] = 0xc9;  // RET
    }

    if (image_.snapshotBlocks()) {
        data[jpPos_ - 1] = 0x040;
        data[jpPos_] = 0xc9;
        const std::uint8_t* tail = k7zx::data::load_snap_blocks;
        std::size_t tailLen = CodeSizes::load_snap_blocks;
        if (method == kMilks) {
            tail = k7zx::data::load_snap_blocks_milks;
            tailLen = CodeSizes::load_snap_blocks_milks;
        } else if (method == kManchester || method == kManchesterDif) {
            tail = k7zx::data::load_snap_blocks_manchester;
            tailLen = CodeSizes::load_snap_blocks_manchester;
        }
        std::memcpy(data + longLineLength_, tail, tailLen);
        longLineLength_ += static_cast<unsigned>(tailLen);
    } else {
        std::size_t n;
        if (method == kMilks)
            n = 4 + totalBlocks_ + 1;
        else if (method == kManchester || method == kManchesterDif)
            n = 1 + 8 * totalBlocks_ + 1;
        else
            n = 1 + 6 * totalBlocks_ + 1;
        n = std::min(n, multiloadCode_.size());
        std::memcpy(data + longLineLength_, multiloadCode_.data(), n);
        longLineLength_ += static_cast<unsigned>(n);
    }
    finishBasic();

    for (int t = 0; t < kMaxConvertBlocks; ++t) {
        BlockInfo& z = image_.blocks()[static_cast<std::size_t>(t)];
        if (z.isTerminator()) break;
        if (!z.selected || z.data == nullptr) continue;
        unsigned ini, largo;
        if (z.type == '0') {
            ini = kLoaderBase;
            largo = 60 + z.length;
        } else {
            ini = z.startAddress;
            largo = z.length;
            std::uint8_t* mem = image_.memory().data();
            // The same bound as the copy below, but for the encoder: it walks
            // `largo` bytes from `mem + ini` whether or not the copy happened.
            if (static_cast<std::size_t>(ini) + largo > TapeImage::kMemorySize) {
                largo = ini >= TapeImage::kMemorySize
                            ? 0
                            : static_cast<unsigned>(TapeImage::kMemorySize - ini);
            }
            if (static_cast<std::size_t>(ini) + largo <= TapeImage::kMemorySize)
                std::memcpy(mem + ini, z.data, largo);
        }
        applyPokes();
        writer_->renderEncodedBlock(image_.memory().data() + ini, static_cast<unsigned short>(ini),
                             static_cast<unsigned short>(largo));
    }
}

void HighSpeedConverter::convertOriginalLoader() {
    buildMultiblock();

    startBasic();
    std::uint8_t* data = basicTap_.data() + lineOffset_;

    poke16(data + 22 + 11, 0, 23618);  // NEWPPC
    pointPreambleAtDispatcher(data);

    if (loaderOpts_.controlChecksum) {
        data[jpPos_] = 0xC8;
        data[jpPos_ + 1] = 0xCF;
        data[jpPos_ + 2] = 0x1A;
    } else {
        data[jpPos_] = 0xc9;
    }

    // The original used `strcat(data, lprint_code); longitud = strlen(data);`.
    // strcat lands on the first NUL -- the end of the loader body -- and
    // strlen then runs on through the shim, so the shim is part of the BASIC
    // line.  It has to be: the rewritten `LOAD "" CODE` statements become
    // LPRINTs, and this shim is what points the printer channel at the Milks
    // loader.  (An earlier version capped the length at the body, dropping
    // the shim, so this scheme never loaded anything.)
    std::size_t end = 0;
    while (end < basicTap_.size() - lineOffset_ && data[end] != 0) ++end;       // strcat
    std::memcpy(data + end, k7zx::data::lprint_code, CodeSizes::lprint_code);
    std::size_t n = end;
    while (n < basicTap_.size() - lineOffset_ && data[n] != 0) ++n;             // strlen
    longLineLength_ = static_cast<unsigned>(n);

    finishBasic();

    for (int t = 0; t < kMaxConvertBlocks; ++t) {
        BlockInfo& z = image_.blocks()[static_cast<std::size_t>(t)];
        if (z.isTerminator()) break;

        writer_->pilotDigital(2500);

        if (!z.selected) continue;
        if (z.type == '0') {
            std::uint8_t* mem = image_.memory().data();
            std::memcpy(mem + kLoaderBase, z.data, std::min<std::size_t>(z.length, TapeImage::kMemorySize - kLoaderBase));
            changeLoadCode(mem + kLoaderBase, z.length);
            writer_->renderEncodedBlock(mem + kLoaderBase, static_cast<unsigned short>(z.startAddress),
                                 static_cast<unsigned short>(60 + z.length));
        } else {
            writer_->renderEncodedBlock(z.data, z.startAddress, z.length);
        }
    }
}

// ---------------------------------------------------------------------------
// Rayo (added by this port; not part of k7zx 4.3)
// ---------------------------------------------------------------------------
void HighSpeedConverter::writeBasicLine(const std::vector<std::uint8_t>& line) {
    // The same two blocks finishBasic() writes: a program header (autostart
    // line 0, no variables) and the program itself.
    const unsigned len = static_cast<unsigned>(line.size());
    std::vector<std::uint8_t> hdr(19, 0);
    hdr[0] = 0x00;  // header flag
    hdr[1] = 0x00;  // program
    const std::string name = tapeName();
    std::memcpy(hdr.data() + 2, name.data(), 10);
    poke16(hdr.data(), 12, static_cast<std::uint16_t>(len));  // length
    poke16(hdr.data(), 14, 0);                                // LINE 0
    poke16(hdr.data(), 16, static_cast<std::uint16_t>(len));  // program length
    std::vector<std::uint8_t> data;
    data.reserve(len + 2);
    data.push_back(0xff);
    data.insert(data.end(), line.begin(), line.end());
    data.push_back(0);  // parity, filled in by loaderBlock()
    writer_->loaderBlock(hdr.data(), static_cast<unsigned>(hdr.size()), audioOpts_.accelerateBasic);
    writer_->loaderBlock(data.data(), static_cast<unsigned>(data.size()), audioOpts_.accelerateBasic);
}

void HighSpeedConverter::convertRayo() {
    rayo::Request rq;
    rq.sampleRate = audioOpts_.sampleRate;
    rq.pmin = audioOpts_.samplesPerBit == kS2_25 ? 3 : audioOpts_.samplesPerBit == kS2_75 ? 4 : 0;
    rq.compress = loaderOpts_.compress;
    rq.clear = loaderOpts_.clearAddress;
    std::uint8_t* mem = image_.memory().data();
    const unsigned base = rayo::loaderBase();
    const SnapData& snap = image_.snap();
    if (snap.snapshotType == 2) {
        fail("Rayo does not load 128K snapshots yet; Shavings Raudo does\r\n");
        return;
    }
    if (snap.snapshotType == 1) {
        // The RAM the loader occupies is parked in the screen and put back by
        // the restore stub, as for Shavings Raudo, just from a lower address.
        image_.prepareSnapshotMemoryForLoaderAt(base);
        applyPokes();
        rq.data = mem + 0x4000;
        rq.dest = 0x4000;
        rq.length = base - 0x4000;
        rq.usr = loaderOpts_.usrAddress;
        rq.snapshot = true;
    } else {
        joinBlocks();
        applyPokes();
        unsigned end = newEnd_;
        if (end > base) {
            warnings_ += "Rayo: data above " + std::to_string(base) +
                         " overlaps the loader and was dropped\r\n";
            end = base;
        }
        if (newStart_ >= end) {
            fail("nothing to load below the Rayo loader\r\n");
            return;
        }
        rq.data = mem + newStart_;
        rq.dest = newStart_;
        rq.length = end - newStart_;
        rq.usr = loaderOpts_.usrAddress;
    }
    rayo::Program prog;
    std::string err;
    if (!rayo::build(rq, prog, err)) {
        fail(err + "\r\n");
        return;
    }
    warnings_ += prog.warnings;
    if (loaderOpts_.generateLoader) writeBasicLine(prog.line);
    writer_->renderCycles(prog.cycles);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
bool HighSpeedConverter::convert(const std::string& outPath) {
    error_.clear();
    duration_ = -1;

    writer_ = std::make_unique<AudioWriter>(audioOpts_);
    writer_->setSnapshotBlocks(image_.snapshotBlocks());
    writer_->setDeltaDurations(deltaDurations_);

    if (!writer_->open(outPath)) {
        fail("error opening output file\r\n" + writer_->error());
        return false;
    }

    try {
        const SnapData& snap = image_.snap();
        if (static_cast<Method>(audioOpts_.method) == kRayo) {
            convertRayo();
        } else if (snap.snapshotType == 1) {
            convertSnapshot48K();
        } else if (snap.snapshotType == 2) {
            convertSnapshot128K();
        } else {
            switch (loaderOpts_.scheme) {
                case kOneBlock: convertSingleBlock(); break;
                case kOriginalLoader: convertOriginalLoader(); break;
                case kManyBlocks:
                case kSnapshot:
                default: convertMultiBlock(); break;
            }
        }
    } catch (const std::exception& e) {
        fail(std::string("internal error: ") + e.what());
    }

    writer_->close();
    if (!writer_->error().empty()) fail(writer_->error());
    duration_ = writer_->duration();
    return error_.empty();
}

// ===========================================================================
// NormalConverter
// ===========================================================================
NormalConverter::NormalConverter(const AudioOptions& audio) : audioOpts_(audio) {}

bool NormalConverter::convert(const std::string& inPath, const std::string& outPath) {
    error_.clear();
    warnings_.clear();
    duration_ = -1;

    // The original forced 44.1 kHz for this path regardless of the setting.
    AudioOptions opts = audioOpts_;
    opts.sampleRate = 44100;
    opts.method = kRom;
    writer_ = std::make_unique<AudioWriter>(opts);

    try {
        filebuf_ = readFile(inPath);
    } catch (const std::exception&) {
        fail("error opening input file\r\n");
        return false;
    }

    if (filebuf_.empty()) {
        fail("error reading input file\r\n");
        return false;
    }

    // PORTING NOTE: the original chose TAP vs TZX purely by looking at the
    // last letter of the file name, so a .sna or .z80 was fed to the TZX
    // walker and produced noise.  Snapshots are memory images, not tapes, so
    // they are rejected here with a clear message.
    const TapeFormat fmt = formatFromPath(inPath);
    if (fmt != TapeFormat::tap && fmt != TapeFormat::tzx) {
        fail("normal mode only reads .tap and .tzx files; use hispeed mode for snapshots\r\n");
        return false;
    }

    if (!writer_->open(outPath)) {
        fail("error opening output file\r\n" + writer_->error());
        return false;
    }

    if (formatFromPath(inPath) == TapeFormat::tap)
        convertTap();
    else
        convertTzx();

    writer_->close();
    if (!writer_->error().empty()) fail(writer_->error());
    duration_ = writer_->duration();
    return error_.empty();
}

void NormalConverter::convertTap() {
    const ByteReader r(filebuf_);
    std::size_t offset = 0;
    while (offset < filebuf_.size()) {
        if (!r.has(offset, 2)) {
            warn("truncated block in .tap file\r\n");
            break;
        }
        const std::uint16_t len = r.le16(offset);
        if (offset + 2 + len > filebuf_.size()) {
            warn("block overruns end of .tap file\r\n");
            break;
        }
        const std::uint8_t flag = r.u8(offset + 2);
        if (flag == 0x00)
            writer_->pureTone(2168 / kTsPerSample, 8064);   // header: long pilot
        else if (flag == 0xff)
            writer_->pureTone(2168 / kTsPerSample, 3220);   // data: short pilot
        else {
            fail("error .tap file format\r\n");
            break;
        }
        writer_->sync(667 / kTsPerSample, 735 / kTsPerSample);
        writer_->pureData(855 / kTsPerSample, 1710 / kTsPerSample, r.ptr(offset + 2, len),
                          len, 8);
        writer_->pause(1710 / kTsPerSample, 1000);
        offset += 2 + len;
    }
}

void NormalConverter::convertTzx() {
    const ByteReader r(filebuf_);
    std::size_t offset = 10;  // TZX header
    while (offset < filebuf_.size()) {
        if (!r.has(offset, 1)) break;
        switch (filebuf_[offset]) {
            case 0x10: {  // standard speed data
                if (!r.has(offset, 5)) { fail("truncated 0x10 block\r\n"); return; }
                const std::uint16_t tapLen = r.le16(offset + 3);
                if (offset + 5 + tapLen > filebuf_.size()) { fail("0x10 overruns file\r\n"); return; }
                const std::uint8_t flag = r.u8(offset + 5);
                if (flag == 0x00)
                    writer_->pureTone(2168 / kTsPerSample, 8064);
                else if (flag == 0xff)
                    writer_->pureTone(2168 / kTsPerSample, 3220);
                else {
                    fail("error .tzx file format\r\n");
                    return;
                }
                writer_->sync(667 / kTsPerSample, 735 / kTsPerSample);
                writer_->pureData(855 / kTsPerSample, 1710 / kTsPerSample, r.ptr(offset + 5, tapLen),
                                  tapLen, 8);
                writer_->pause(1710 / kTsPerSample, r.le16(offset + 1));
                offset += 5 + tapLen;
                break;
            }
            case 0x11: {  // turbo data
                if (!r.has(offset, 19)) { fail("truncated 0x11 block\r\n"); return; }
                const std::uint16_t l1 = r.le16(offset + 1);
                const std::uint16_t lSync1 = r.le16(offset + 3);
                const std::uint16_t lSync2 = r.le16(offset + 5);
                const std::uint16_t lZero = r.le16(offset + 7);
                const std::uint16_t lOne = r.le16(offset + 9);
                const std::uint16_t nPilot = r.le16(offset + 11);
                const int usedBits = filebuf_[offset + 13];
                const std::uint16_t pauseMs = r.le16(offset + 14);
                // 3-byte little-endian data length at offset 16 (see the note
                // in TapeImage::readTzx about the original's decoding of it).
                // 3-byte little-endian, i.e. low16 + 0x10000 * third -- see TapeImage::readTzx.
    const std::uint32_t dataLen = r.le16(offset + 16) + 0x10000u * filebuf_[offset + 18];
                if (offset + 19 + dataLen > filebuf_.size()) { fail("0x11 overruns file\r\n"); return; }
                writer_->pureTone(l1 / kTsPerSample, nPilot);
                writer_->sync(lSync1 / kTsPerSample, lSync2 / kTsPerSample);
                writer_->pureData(lZero / kTsPerSample, lOne / kTsPerSample,
                                  r.ptr(offset + 19, dataLen), static_cast<int>(dataLen), usedBits);
                writer_->pause(lOne / kTsPerSample, pauseMs);
                offset += 19 + dataLen;
                break;
            }
            case 0x12: {  // pure tone
                if (!r.has(offset, 5)) { fail("truncated 0x12 block\r\n"); return; }
                writer_->pureTone(r.le16(offset + 1) / kTsPerSample, r.le16(offset + 3));
                offset += 5;
                break;
            }
            case 0x13: {  // pulse sequence
                if (!r.has(offset, 2)) { fail("truncated 0x13 block\r\n"); return; }
                const int n = filebuf_[offset + 1];
                if (offset + 2 + 2 * static_cast<std::size_t>(n) > filebuf_.size()) {
                    fail("0x13 overruns file\r\n");
                    return;
                }
                std::vector<std::uint16_t> pulses(static_cast<std::size_t>(n));
                for (int i = 0; i < n; ++i) pulses[static_cast<std::size_t>(i)] = r.le16(offset + 2 + 2 * i);
                writer_->sequence(pulses.data(), n);
                offset += 2 + 2 * static_cast<std::size_t>(n);
                break;
            }
            case 0x14: {  // pure data
                if (!r.has(offset, 11)) { fail("truncated 0x14 block\r\n"); return; }
                const std::uint16_t lZero = r.le16(offset + 1);
                const std::uint16_t lOne = r.le16(offset + 3);
                const int usedBits = filebuf_[offset + 5];
                const std::uint16_t pauseMs = r.le16(offset + 6);
                const std::uint32_t dataLen = r.le16(offset + 8) + 0x10000u * filebuf_[offset + 10];
                if (offset + 11 + dataLen > filebuf_.size()) { fail("0x14 overruns file\r\n"); return; }
                writer_->pureData(lZero / kTsPerSample, lOne / kTsPerSample,
                                  r.ptr(offset + 11, dataLen), static_cast<int>(dataLen), usedBits);
                writer_->pause(lZero / kTsPerSample, pauseMs);
                offset += 11 + dataLen;
                break;
            }
            case 0x15: {  // direct recording
                if (!r.has(offset, 9)) { fail("truncated 0x15 block\r\n"); return; }
                const std::uint16_t tstates = r.le16(offset + 1);
                const std::uint16_t pauseMs = r.le16(offset + 3);
                const std::uint32_t dataLen = r.le16(offset + 6) + 0x10000u * filebuf_[offset + 8];
                if (offset + 9 + dataLen > filebuf_.size()) { fail("0x15 overruns file\r\n"); return; }
                writer_->directRecording(tstates, r.ptr(offset + 9, dataLen), static_cast<int>(dataLen));
                writer_->pause(1500 / kTsPerSample, pauseMs);
                offset += 9 + dataLen;
                break;
            }
            case 0x18:
            case 0x19:
                if (!r.has(offset, 5)) { fail("truncated 0x18 block\r\n"); return; }
                warn("k7zx can't read 0x18 0x19 tzx blocks\r\n");
                offset += 5 + r.le32(offset + 1);
                break;
            case 'Z': offset += 10; break;
            case 0x20: offset += 3; break;
            case 0x21: if (!r.has(offset, 2)) return; offset += 1 + filebuf_[offset + 1] + 1; break;
            case 0x22: offset += 1; break;
            case 0x24: offset += 3; break;
            case 0x25: offset += 1; break;
            case 0x26: if (!r.has(offset, 3)) return; offset += 3 + 2u * r.le16(offset + 1); break;
            case 0x27: offset += 1; break;
            case 0x28: if (!r.has(offset, 3)) return; offset += 3 + r.le16(offset + 1); break;
            case 0x2a: offset += 4; break;
            case 0x2b: offset += 5; break;
            case 0x30: if (!r.has(offset, 2)) return; offset += 1 + filebuf_[offset + 1] + 1; break;
            case 0x31: if (!r.has(offset, 5)) return; offset += 5 + r.le32(offset + 1); break;
            case 0x32:
                if (!r.has(offset, 3)) return;
                offset += 1 + filebuf_[offset + 1] + 0x100u * filebuf_[offset + 2] + 2;
                break;
            case 0x33: if (!r.has(offset, 2)) return; offset += 1 + 3u * filebuf_[offset + 1] + 1; break;
            case 0x34: offset += 9; break;
            case 0x35: if (!r.has(offset, 19)) return; offset += 19 + r.le32(offset + 15); break;
            case 0x40:
                if (!r.has(offset, 5)) return;
                offset += 5 + r.le32(offset + 1);
                break;
            default:
                fail("error .tzx file format\r\n");
                return;
        }
        if (offset > filebuf_.size()) {
            fail("tzx block overruns end of file\r\n");
            return;
        }
    }
    writer_->pause(1710 / kTsPerSample, 1000);
}

}  // namespace k7zx
