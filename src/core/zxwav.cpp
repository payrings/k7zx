// k7zx 5.0 - modern C++ port
//
// The signal generator, ported from zxwav.cpp.
#include "zxwav.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "byteorder.h"

namespace k7zx {
namespace {


/// Digital pilot tone pulse widths (`_t_P1_` .. `_t_S2_`).
/// Longest single pulse, in samples, that a direct-recording block may claim.
/// A real ZX pulse is about 2168 T-states -- 27 samples -- so one second is a
/// ~1700x margin.  It exists because `tstates-per-pulse` is an unchecked 16-bit
/// file field and the pulse is measured in *bits*: a payload of 4 KB of one
/// repeated byte is a single 32768-bit pulse, which at 0xFFFF T-states per bit
/// asked for ten minutes of audio out of a 4 KB file.  Note the total is still
/// proportional to the input size; this bounds the per-pulse factor, not the product.
constexpr long long kMaxPulseSamples = 48000ll;

constexpr int kTP1 = 8;
constexpr int kTP2 = 8;
constexpr int kTS1 = 4;
constexpr int kTS2 = 2;

/// Scale factor that flattens a long pulse back to unit amplitude.  The
/// original used a lookup table indexed by pulse length; the table contents
/// are reproduced verbatim below -- 70 entries, being 1, 1, 0.7071, 0.5774,
/// 0.5, twenty-one 0.4472 (indices 5..25), then 0.4082, 0.378, 0.3536, 0.3333
/// and forty 0.3162.  An extra 0.4472 here shifts everything from index 26 up
/// by one slot, so the count is asserted rather than left to inspection.
const double kInvSqrt[] = {
    1.0    , 1.0    , 0.7071 , 0.5774 , 0.5    , 0.4472 , 0.4472 , 0.4472 , 0.4472,
    0.4472 , 0.4472 , 0.4472 , 0.4472 , 0.4472 , 0.4472 , 0.4472 , 0.4472 , 0.4472,
    0.4472 , 0.4472 , 0.4472 , 0.4472 , 0.4472 , 0.4472 , 0.4472 , 0.4472 , 0.4082,
    0.378  , 0.3536 , 0.3333 , 0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162,
    0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162,
    0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162,
    0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162,
    0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162 , 0.3162};
static_assert(std::size(kInvSqrt) == 70,
              "kInvSqrt must hold the original's 70 entries: an extra one shifts "
              "every index from 26 up");

double invSqrt(std::size_t n) {
    const std::size_t last = sizeof(kInvSqrt) / sizeof(kInvSqrt[0]) - 1;
    return kInvSqrt[n <= last ? n : last];
}

std::uint8_t xorChecksum(const std::uint8_t* s, int length) {
    std::uint8_t res = 0;
    for (int i = 0; i < length; ++i) res ^= s[i];
    return res;
}

}  // namespace

AudioWriter::AudioWriter(const AudioOptions& opt) : opt_(opt) {
    scratch_.reserve(1024);
    stereoInterleaved_.reserve(2048);
    mono_.reserve(1024);
}

AudioWriter::~AudioWriter() {
    if (f_) std::fclose(f_);
}

// ---------------------------------------------------------------------------
// Container setup
// ---------------------------------------------------------------------------
bool AudioWriter::open(const std::string& path) {
    duration_ = -1;
    db_ = 115;
    dataBytes_ = 0;
    tzxBytes_ = 0;
    nBit_ = 1;
    phase_ = 0;
    error_.clear();

    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) {
        error_ = "cannot open '" + path + "' for writing";
        return false;
    }

    const int rate = opt_.effectiveRate();

    if (opt_.emulate) {
        // TZX 1.19 header + a "text description" block + a direct recording
        // block whose length we patch in on close().
        ByteWriter h;
        // PORTING NOTE: the original declared the magic as `char zx_tape[7]`
        // and then strcpy'd the eight byte "ZXTape!" into it, so the file it
        // produced started "ZXTa" 0x1A and had the 0x30 text block ID at offset
        // 7 -- not a file any TZX reader accepts.
        //
        // The header is 12 bytes: the magic, 0x1A, major, minor, and a two byte
        // "ID of first block".  k7zx 4.3 stopped after the minor version
        // (sizeof(st_head_tzx) was 10) and so wrote -- and read -- a header two
        // bytes short, which no other TZX tool can make sense of.  Both ends of
        // k7zx now write and accept the full 12.
        h.raw("ZXTape!", 7);
        h.u8(0x1a);
        h.u8(1);   // major
        h.u8(19);  // minor
        h.le16(0x0030);  // ID of first block: the text description below
        h.u8(0x30);
        h.u8(50);
        // Zero-initialised: snprintf only writes the formatted text and its NUL,
        // but 50 bytes are stored below, so the unwritten tail used to be
        // whatever was on the stack -- 16 bytes of the program's memory, and it
        // made two identical --emulate runs produce different files.
        char msg[51] = {};
        std::snprintf(msg, sizeof msg, "Block from k7zx encoding:%2.2i s/b:%2.2i", opt_.method,
                      opt_.samplesPerBit);
        h.raw(msg, 50);
        h.u8(0x15);
        h.le16(static_cast<std::uint16_t>(0.5 + kUlaHz / rate));
        h.le16(100);  // pause after block, ms
        h.u8(0);      // used bits in last byte
        // 3 byte little-endian data length, patched in close()
        h.le16(0);
        h.u8(0);
        tzxHeader_ = std::move(h.buffer());
        tzxDataOffset_ = tzxHeader_.size();
        const std::size_t n = std::fwrite(tzxHeader_.data(), 1, tzxHeader_.size(), f_);
        if (n != tzxHeader_.size()) {
            error_ = "short write on TZX header";
            return false;
        }
    } else {
        blockAlign_ = opt_.stereo ? 4 : 1;
        writeContainerHeader();
    }
    return true;
}

void AudioWriter::writeContainerHeader() {
    ByteWriter w;
    w.tag("RIFF");
    w.le32(40);  // patched on close
    w.tag("WAVE");
    w.tag("fmt ");
    w.le32(0x10);
    w.le16(1);  // PCM
    w.le16(static_cast<std::uint16_t>(opt_.stereo ? 2 : 1));
    w.le32(static_cast<std::uint32_t>(opt_.effectiveRate()));
    w.le32(static_cast<std::uint32_t>(opt_.effectiveRate() * blockAlign_));
    w.le16(blockAlign_);
    w.le16(static_cast<std::uint16_t>(opt_.stereo ? 16 : 8));
    w.tag("data");
    w.le32(0);  // patched on close
    const std::size_t n = std::fwrite(w.buffer().data(), 1, w.buffer().size(), f_);
    if (n != w.buffer().size()) error_ = "short write on WAV header";
}

void AudioWriter::patchContainerHeader() {
    if (!f_) return;
    std::fseek(f_, 0, SEEK_SET);
    ByteWriter w;
    w.tag("RIFF");
    w.le32(dataBytes_ + 36);
    w.tag("WAVE");
    w.tag("fmt ");
    w.le32(0x10);
    w.le16(1);
    w.le16(static_cast<std::uint16_t>(opt_.stereo ? 2 : 1));
    w.le32(static_cast<std::uint32_t>(opt_.effectiveRate()));
    w.le32(static_cast<std::uint32_t>(opt_.effectiveRate() * blockAlign_));
    w.le16(blockAlign_);
    w.le16(static_cast<std::uint16_t>(opt_.stereo ? 16 : 8));
    w.tag("data");
    w.le32(dataBytes_);
    const std::size_t n = std::fwrite(w.buffer().data(), 1, w.buffer().size(), f_);
    if (n != w.buffer().size()) error_ = "short write on WAV header";
}

bool AudioWriter::close() {
    if (!f_) return false;

    // Decaying final tone, exactly as the original appended it.
    if (opt_.finalTone || opt_.stereo) {
        for (int t = 0; t < 1000; ++t) {
            db_ = 100000 * db_ / (100000.0 + t);
            analogCycle(24);
        }
    }

    std::fseek(f_, 0, SEEK_SET);
    if (opt_.emulate) {
        // The block header ends with its 3 byte little-endian data length.
        tzxHeader_[tzxDataOffset_ - 3] = static_cast<std::uint8_t>(tzxBytes_ & 0xff);
        tzxHeader_[tzxDataOffset_ - 2] = static_cast<std::uint8_t>((tzxBytes_ >> 8) & 0xff);
        tzxHeader_[tzxDataOffset_ - 1] = static_cast<std::uint8_t>((tzxBytes_ >> 16) & 0xff);
        duration_ = (tzxBytes_ * 8.0) / opt_.effectiveRate();
        if (std::fwrite(tzxHeader_.data(), 1, tzxHeader_.size(), f_) != tzxHeader_.size())
            error_ = "short write on TZX header";
    } else {
        patchContainerHeader();
        duration_ = static_cast<double>(dataBytes_ / blockAlign_) / opt_.effectiveRate();
    }
    std::fclose(f_);
    f_ = nullptr;
    return error_.empty();
}

// ---------------------------------------------------------------------------
// Core sample emitters
// ---------------------------------------------------------------------------
int AudioWriter::emit(const int* y, int n) {
    if (n <= 0) return 0;
    const int sign = opt_.invert ? -1 : 1;

    if (opt_.emulate) {
        // Bit-pack into a TZX direct-recording block.  A leading 1 in every
        // byte is the TZX convention for "first sample is high".
        int written = 0;
        for (int i = 0; i < n; ++i) {
            nBit_ <<= 1;
            if ((sign * y[i]) > 0) ++nBit_;
            if (nBit_ > 0x100) {
                const std::uint8_t byte = static_cast<std::uint8_t>(nBit_ & 0xff);
                written += static_cast<int>(std::fwrite(&byte, 1, 1, f_));
                ++tzxBytes_;
                nBit_ = 1;
            }
        }
        return written;
    }

    if (opt_.stereo) {
        // The two channels must be interleaved, not written as two blocks.
        // The original used an array of {l, r} structs and a single fwrite,
        // which interleaves; writing two buffers in sequence instead shifts the
        // right channel by the whole length of the file, so the loader plays
        // in one ear and the payload in the other.
        stereoInterleaved_.resize(static_cast<std::size_t>(n) * 2);
        const int rightSign = opt_.invertRight ? -1 : 1;
        for (int i = 0; i < n; ++i) {
            const int v = 0x100 * y[i] * (-sign);
            stereoInterleaved_[static_cast<std::size_t>(i) * 2] = static_cast<std::int16_t>(v);
            stereoInterleaved_[static_cast<std::size_t>(i) * 2 + 1] =
                static_cast<std::int16_t>(v * rightSign);
        }
        const int res = static_cast<int>(std::fwrite(stereoInterleaved_.data(), 1,
                                                      static_cast<std::size_t>(n) * 4, f_));
        dataBytes_ += static_cast<std::uint32_t>(n) * 4;
        return res;
    }

    mono_.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        mono_[static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(y[i] * sign + 0x80);
    }
    const int res = static_cast<int>(std::fwrite(mono_.data(), 1, static_cast<std::size_t>(n), f_));
    dataBytes_ += static_cast<std::uint32_t>(n);
    return res;
}

int AudioWriter::digitalCycle(int t) {
    const int total = t;
    if (total <= 0) return 0;
    const int n0 = total / 2;
    const int n1 = total - n0;
    scratch_.resize(static_cast<std::size_t>(total));

    int i = 0;
    float amp = 0.0f, y = 0.0f, x = 0.0f;
    switch (opt_.waveform) {
        case kSquare:
        default:
            for (; i < n0; ++i) scratch_[static_cast<std::size_t>(i)] = -113;
            for (; i < n0 + n1; ++i) scratch_[static_cast<std::size_t>(i)] = 113;
            break;
        case kRamp: {
            amp = 600 * static_cast<float>(invSqrt(static_cast<std::size_t>(total))) /
                  static_cast<float>(t);
            for (int j = 0; j < total; ++j) {
                x = j + 0.5f;
                y = amp * (x - total / 2.0f);
                scratch_[static_cast<std::size_t>(j)] = static_cast<int>(y);
            }
            break;
        }
        case kCubic:
            amp = -2260.0f;
            for (int j = 0; j < total; ++j) {
                x = j + 1;
                y = (amp * (x - 0.5f) * (x - total - 0.5f) * (x - total / 2.0f - 0.5f)) /
                    (static_cast<float>(total) * total * total);
                scratch_[static_cast<std::size_t>(j)] = static_cast<int>(y);
            }
            break;
        case kZeroContinuous:
            for (; i < n0; ++i)
                scratch_[static_cast<std::size_t>(i)] = static_cast<int>(-113.0 / n0);
            for (; i < n0 + n1; ++i)
                scratch_[static_cast<std::size_t>(i)] = static_cast<int>(113.0 / n1);
            break;
        case kEqualEnergy:
            for (; i < n0; ++i)
                scratch_[static_cast<std::size_t>(i)] =
                    static_cast<int>(-113 * invSqrt(static_cast<std::size_t>(n0)));
            for (; i < n0 + n1; ++i)
                scratch_[static_cast<std::size_t>(i)] =
                    static_cast<int>(113 * invSqrt(static_cast<std::size_t>(n1)));
            break;
        case kParabola: {
            double a = (490.0 * invSqrt(static_cast<std::size_t>(n0))) / (static_cast<double>(n0) * n0);
            for (int j = -n0; j < 0; ++j) {
                x = j + 0.6f;
                y = static_cast<float>(a * x * (x + n0));
                scratch_[static_cast<std::size_t>(j + n0)] = static_cast<int>(y);
            }
            a = (490.0 * invSqrt(static_cast<std::size_t>(n1))) / (static_cast<double>(n1) * n1);
            for (int j = 0; j < n1; ++j) {
                x = j + 0.4f;
                y = static_cast<float>(-a * x * (x - n1));
                scratch_[static_cast<std::size_t>(j + n0)] = static_cast<int>(y);
            }
            break;
        }
        case kDelta:
            scratch_[0] = -113;
            for (int j = 1; j < n0 + n1; ++j) scratch_[static_cast<std::size_t>(j)] = 113;
            break;
    }
    return emit(scratch_.data(), total);
}

int AudioWriter::digitalCycle(int low, int high) {
    const int n0 = low;
    const int n1 = high;
    const int total = n0 + n1;
    if (total <= 0) return 0;
    scratch_.resize(static_cast<std::size_t>(total));

    int i = 0;
    double amp = 0, x = 0, y = 0;
    switch (opt_.waveform) {
        case kSquare:
        default:
            for (; i < n0; ++i) scratch_[static_cast<std::size_t>(i)] = -113;
            for (; i < total; ++i) scratch_[static_cast<std::size_t>(i)] = 113;
            break;
        case kRamp:
            for (; i < n0; ++i)
                scratch_[static_cast<std::size_t>(i)] = -113 + 8 * i;
            for (; i < total; ++i)
                scratch_[static_cast<std::size_t>(i)] = 113 - 8 * (i - n0);
            break;
        case kCubic: {
            amp = 480.0 / (static_cast<double>(n0) * n0);
            for (i = -n0; i < 0; ++i) {
                x = i + 0.7;
                y = amp * x * (x + n0);
                scratch_[static_cast<std::size_t>(i + n0)] = static_cast<int>(y);
            }
            amp = 480.0 / (static_cast<double>(n1) * n1);
            for (; i < n1; ++i) {
                x = i + 0.3;
                y = -amp * x * (x - n1);
                scratch_[static_cast<std::size_t>(i + n0)] = static_cast<int>(y);
            }
            break;
        }
        case kZeroContinuous:
            for (; i < n0; ++i)
                scratch_[static_cast<std::size_t>(i)] = static_cast<int>(-113.0 / n0);
            for (; i < total; ++i)
                scratch_[static_cast<std::size_t>(i)] = static_cast<int>(113.0 / n1);
            break;
        case kEqualEnergy:
            for (; i < n0; ++i)
                scratch_[static_cast<std::size_t>(i)] =
                    static_cast<int>(-113 * invSqrt(static_cast<std::size_t>(n0)));
            for (; i < total; ++i)
                scratch_[static_cast<std::size_t>(i)] =
                    static_cast<int>(113 * invSqrt(static_cast<std::size_t>(n1)));
            break;
        case kParabola: {
            amp = (490.0 * invSqrt(static_cast<std::size_t>(n0))) / (static_cast<double>(n0) * n0);
            for (i = -n0; i < 0; ++i) {
                x = i + 0.6;
                y = amp * x * (x + n0);
                scratch_[static_cast<std::size_t>(i + n0)] = static_cast<int>(y);
            }
            amp = (490.0 * invSqrt(static_cast<std::size_t>(n1))) / (static_cast<double>(n1) * n1);
            for (; i < n1; ++i) {
                x = i + 0.4;
                y = -amp * x * (x - n1);
                scratch_[static_cast<std::size_t>(i + n0)] = static_cast<int>(y);
            }
            break;
        }
    }
    return emit(scratch_.data(), total);
}

int AudioWriter::analogCycle(double p1, double p2) {
    if (p1 <= 0 && p2 <= 0) return 0;
    std::size_t t = 0;
    const std::size_t need = static_cast<std::size_t>(std::ceil(p1 + p2)) + 4;
    if (scratch_.size() < need) scratch_.resize(need);

    double amp = (db_ * 20) / (8 * p1 * p1 * p1);
    for (double x = phase_; x < p1; x += 1.0)
        scratch_[t++] = static_cast<int>(std::floor(0.5 - amp * (x * (x - p1) * (x - 2 * p1))));
    phase_ = static_cast<double>(t) + phase_ - p1;
    const std::size_t t1 = t;

    amp = (db_ * 20) / (8 * p2 * p2 * p2);
    for (double x = phase_; x < p2; x += 1.0)
        scratch_[t++] = static_cast<int>(std::floor(0.5 - amp * (x * (x + p2) * (x - p2))));
    phase_ = static_cast<double>(t) - static_cast<double>(t1) + phase_ - p2;

    return emit(scratch_.data(), static_cast<int>(t));
}

int AudioWriter::analogCycle(double p) { return analogCycle(p / 2, p / 2); }

void AudioWriter::buildWaveTable(int* f, int t) {
    f[0] = t;
    const double amp = (-db_ * 0x7a) / 115.0;
    for (int j = 0; j < t; ++j) {
        const double x = j + 1;
        const double y = -std::sin((x - t / 2.0 - 0.45) * 2 * M_PI / t);
        f[1 + j] = static_cast<int>(y < 0 ? -amp * std::sqrt(-y) : amp * std::sqrt(y));
    }
}

// ---------------------------------------------------------------------------
// Tape events
// ---------------------------------------------------------------------------
void AudioWriter::pilotDigital(int n) {
    db_ = 115;
    for (int i = 0; i < n; ++i) {
        if (kTP1 == kTP2)
            digitalCycle(kTP1 + kTP2);
        else
            digitalCycle(kTP1, kTP2);
    }
}


void AudioWriter::pureTone(double pilot, int n) {
    int i = 0;
    if (n & 1) {
        // Odd pulse count: emit a single half cycle, flipping the amplitude
        // so the tone starts and ends at the same level.
        analogCycle(pilot);
        db_ = -db_;
        i = 1;
    }
    for (; i < n; i += 2) analogCycle(pilot, pilot);
}

void AudioWriter::sync(double sync1, double sync2) { analogCycle(sync1, sync2); }

void AudioWriter::sequence(const std::uint16_t* pulses, int n) {
    int i = 0;
    if (n & 1) {
        analogCycle(pulses[0] / kTsPerSample);
        db_ = -db_;
        i = 1;
    }
    for (; i < n; i += 2)
        analogCycle(pulses[i] / kTsPerSample, pulses[i + 1] / kTsPerSample);
}

void AudioWriter::pureData(double zero, double one, const std::uint8_t* d, int n, int usedBits) {
    for (int i = 0; i < n - 1; ++i) {
        std::uint8_t b = 0x80;
        while (b) {
            if (b & d[i])
                analogCycle(one, one);
            else
                analogCycle(zero, zero);
            b >>= 1;
        }
    }
    if (n <= 0) return;
    std::uint8_t b = 0x80;
    for (int i = 0; i < usedBits; ++i) {
        if (b & d[n - 1])
            analogCycle(one, one);
        else
            analogCycle(zero, zero);
        b >>= 1;
    }
}

void AudioWriter::pause(double p, int ms) {
    if (ms == 0) return;
    if (db_ < 0) {
        // alonepulse_analogico, which this stands in for, ends with `db=-db`
        // and the original reads `m = db` *after* that flip.  Without the flip
        // `saved` is negative, the decay loop clamps it to 1 and the pause is
        // rendered as digital silence, and the negative value is then restored
        // so every following block in the file comes out mirrored.
        analogCycle(p);
        db_ = -db_;
    }
    analogCycle(p, p);
    const double saved = db_;
    p = 250;
    // 44100 * ms overflows int above ~48.7 s, which made the bound negative and
    // dropped the pause to its two opening cycles.  A TZX pause is a 16-bit
    // field, so this is reachable from a file.
    const double total = 44100.0 * ms / (p * 2000.0);
    for (int i = 1; static_cast<double>(i) < total; ++i) {
        db_ = 200 * db_ / (200.0 + i);
        if (db_ < 1) db_ = 1;
        analogCycle(p, p);
    }
    db_ = saved;
}

void AudioWriter::directRecording(unsigned shortTstates, const std::uint8_t* data, int n) {
    if (n <= 0) return;
    const bool firstBit = (data[0] & 0x80) != 0;
    // p1/p2 count bits of a run and shortTstates is an unchecked 16-bit field
    // taken straight from the file, so `p1 * shortTstates` was an int
    // multiplication that could overflow -- signed overflow is undefined, and
    // the result is used directly as a sample count.  A 4 KB file could claim
    // half an hour of audio.  Compute in 64 bits and cap the pulse: longer than
    // the cap cannot be rendered anyway.
    const auto pulseSamples = [&](int bits) -> double {
        const long long tstates = static_cast<long long>(bits) * shortTstates;
        if (tstates <= 0) return 0.0;
        const long long samples = tstates / kTsPerSample;
        if (samples > kMaxPulseSamples) return static_cast<double>(kMaxPulseSamples);
        return static_cast<double>(samples);
    };
    int p1 = 0, p2 = 0;
    for (int i = 0; i < n; ++i) {
        std::uint8_t mask = 0x80;
        while (mask) {
            if (firstBit == !(data[i] & mask)) {
                ++p2;
            } else if (p2) {
                analogCycle(pulseSamples(p1), pulseSamples(p2));
                p1 = p2 = 0;
            } else {
                ++p1;
            }
            mask >>= 1;
        }
    }
    if (p2) analogCycle(pulseSamples(p1), pulseSamples(p2));
}

// ---------------------------------------------------------------------------
// Standard-speed loader (the "normal" tap2wav path)
// ---------------------------------------------------------------------------
void AudioWriter::loaderNormal(std::uint8_t* data, unsigned length) {
    const int savedWaveform = opt_.waveform;
    opt_.waveform = kCubic;

      db_ = 115;
      std::vector<int> tone(512), syncPulse(512), zero(512), one(512);
      const auto lengthOf = [&](double tstates) {
          return static_cast<int>(0.5 + tstates * opt_.sampleRate / kUlaHz);
      };
      // Same figures as k7zx 4.3.  buildWaveTable() renders one full square
      // wave *period* -- a low run and a high run -- and the ROM measures each
      // run (edge to edge) as one pulse, so a period of 2*2168 T is two pilot
      // pulses of 2168 T, 2*855 T is the two 855 T pulses of a 0 bit, and so
      // on; the sync period 667+735 T is the 667 T and 735 T sync pulses.
      // (An earlier comment here called this a bug and claimed the figures had
      // been changed; they had not, and must not be: the loader blocks are
      // checked byte for byte against 4.3's own output by the golden test in
      // tests/test_zxcode.cpp, and load through the 48K ROM in tools/zxload.)
      buildWaveTable(tone.data(), lengthOf(2 * 2168));
      buildWaveTable(syncPulse.data(), lengthOf(667 + 735));
      buildWaveTable(zero.data(), lengthOf(2 * 855));
      buildWaveTable(one.data(), lengthOf(2 * 1710));

    data[length - 1] = 0;
    for (unsigned i = 0; i + 1 < length; ++i) data[length - 1] ^= data[i];

    const int nPilot = data[0] ? 1500 : 2500;
    db_ = 1;
    for (int i = 1; i < nPilot; ++i) {
        if (db_ < 115) {
            db_ = i * i / 15;
            if (db_ > 115) db_ = 115;
            buildWaveTable(tone.data(), lengthOf(2 * 2168));
        }
        emit(tone.data() + 1, tone[0]);
    }

    db_ = 115;
    emit(syncPulse.data() + 1, syncPulse[0]);

    for (unsigned i = 0; i < length; ++i) {
        std::uint8_t bit = 0x80;
        while (bit) {
            if (data[i] & bit)
                emit(one.data() + 1, one[0]);
            else
                emit(zero.data() + 1, zero[0]);
            bit >>= 1;
        }
    }

    for (int i = 0; i < 16; ++i) {
        db_ = 16 * db_ / (16.0 + i);
        buildWaveTable(zero.data(), lengthOf(2 * 2168));
        emit(zero.data() + 1, zero[0]);
    }

    opt_.waveform = savedWaveform;
}

void AudioWriter::loaderAccelerated(std::uint8_t* data, unsigned length) {
    const int savedWaveform = opt_.waveform;
    opt_.waveform = kSquare;
    db_ = 115;

    if (opt_.sampleRate == 44100) {
        pulseLengths_[0] = 24; pulseLengths_[1] = 10; pulseLengths_[2] = 8; pulseLengths_[3] = 18;
    } else {
        pulseLengths_[0] = 26; pulseLengths_[1] = 12; pulseLengths_[2] = 8; pulseLengths_[3] = 20;
    }
    data[length - 1] = 0;
    for (unsigned i = 0; i + 1 < length; ++i) data[length - 1] ^= data[i];

      const int nPilot = 1255;
      // As k7zx 4.3: ciclo_digital(tempos[i], tempos[i]), i.e. a run of t
      // samples low and t high per entry.  Each run is one pulse to the ROM, so
      // the pilot entry of 26 samples at 48 kHz is a 1896 T pulse against the
      // nominal 2168 T -- faster, which is the point of the option -- and the
      // bits are 583 T / 1458 T.  (An earlier comment here claimed the port had
      // switched to a single-argument form; it had not.)  Verified to load
      // through the 48K ROM in tools/zxload.
      for (int i = 0; i < nPilot; ++i) digitalCycle(static_cast<int>(pulseLengths_[0]), static_cast<int>(pulseLengths_[0]));
      digitalCycle(static_cast<int>(pulseLengths_[1]), static_cast<int>(pulseLengths_[1]));

      for (unsigned i = 0; i < length; ++i) {
          std::uint8_t bit = 0x80;
          while (bit) {
              if (data[i] & bit)
                  digitalCycle(static_cast<int>(pulseLengths_[3]), static_cast<int>(pulseLengths_[3]));
              else
                  digitalCycle(static_cast<int>(pulseLengths_[2]), static_cast<int>(pulseLengths_[2]));
              bit >>= 1;
          }
    }
    for (int i = 0; i < 25; ++i)
        digitalCycle(static_cast<int>(pulseLengths_[2]), static_cast<int>(pulseLengths_[2]));

    opt_.waveform = savedWaveform;
}

void AudioWriter::loaderBlock(const std::uint8_t* data, unsigned length, bool accelerated) {
    // The loaders XOR a checksum into the last byte, so they need a mutable
    // copy.  Callers hand us a block that is not otherwise modified.
    std::vector<std::uint8_t> block(data, data + length);
    if (accelerated)
        loaderAccelerated(block.data(), length);
    else
        loaderNormal(block.data(), length);

}


// ---------------------------------------------------------------------------
// Per-method block encoders (the original's EncodeXXX functions)
//
// Each of these turns one contiguous memory range into the pulse train that
// the matching Z80 loader in loaderRoutine.cpp can read.  The `muestras_por_bit`
// switch selects the pulse widths; the tables are copied from the original
// verbatim.
// ---------------------------------------------------------------------------

// --- ROM: one bit per pulse, both polarities accepted ---------------------
int AudioWriter::encodeRom(const std::uint8_t* data, unsigned /*startAddress*/, unsigned length) {
    if (opt_.samplesPerBit == kS2_50) {
        digitalCycle(2, 2);
        digitalCycle(2, 2);
        digitalCycle(1, 2);
        for (unsigned x = 0; x < length; ++x) {
            std::uint8_t byteValue = data[x];
            int bitsCount = 8;
            while (bitsCount) {
                digitalCycle(1, 1 + (byteValue > 127));
                byteValue <<= 1;
                --bitsCount;
            }
        }
    } else {
        digitalCycle(4);
        digitalCycle(4);
        digitalCycle(4);
        for (unsigned x = 0; x < length; ++x) {
            std::uint8_t byteValue = data[x];
            int bitsCount = 8;
            while (bitsCount) {
                digitalCycle(2 * (1 + (byteValue > 127)));
                byteValue <<= 1;
                --bitsCount;
            }
        }
    }
    digitalCycle(8);
    digitalCycle(8);
    digitalCycle(16);
    return 0;
}

// --- Milks: 2 bits per pulse, self-describing header ----------------------
int AudioWriter::encodeThrwMilks(const std::uint8_t* data, unsigned startAddress,
                                  unsigned length) {
    digitalCycle(3, 3);

    // The trailing cycle after the loop reuses the last pulse widths.
    int lowWidth = 0, highWidth = 0;

    // chkSum runs across the whole frame: byte 4 is the XOR of the four header
    // bytes before it.  (It used to be declared inside the loop, so byte 4 was
    // always 0 and the loader rejected every block.)
    std::uint8_t chkSum = 0;
    const std::uint8_t dataChk = xorChecksum(data, static_cast<int>(length));
    for (unsigned x = 0; x < length + 6; ++x) {
        std::uint8_t byteValue;
        switch (x) {
            case 0: byteValue = static_cast<std::uint8_t>(startAddress >> 8); break;
            case 1: byteValue = static_cast<std::uint8_t>(startAddress & 0xff); break;
            case 2: byteValue = static_cast<std::uint8_t>((length + 0xff) >> 8); break;
            case 3: byteValue = static_cast<std::uint8_t>((length + 0xff) & 0xff); break;
            case 4: byteValue = chkSum; break;
            case 5: byteValue = dataChk; break;
            default: byteValue = data[x - 6]; break;
        }

        chkSum ^= byteValue;
        int twoBits = 4;
        while (twoBits) {
            highWidth = 2 + ((byteValue & 0xc0) >> 6);
            if (x < 6) {
                lowWidth = highWidth;
            } else {
                switch (opt_.samplesPerBit) {
                    case kS2_75: lowWidth = 2; break;
                    case kS2_50:
                        if (highWidth == 5) highWidth = 1;
                        lowWidth = highWidth;
                        break;
                    case kS2_25:
                        if (highWidth == 5) highWidth = 1;
                        lowWidth = 2;
                        break;
                    case kS1_75:
                        if (highWidth == 5) highWidth = 1;
                        lowWidth = 1;
                        break;
                    case kS3_50:
                    default: lowWidth = highWidth; break;
                }
            }
            digitalCycle(lowWidth, highWidth);
            byteValue <<= 2;
            --twoBits;
        }
    }

    digitalCycle(lowWidth, highWidth);
    digitalCycle(8);
    digitalCycle(16);
    return 0;
}

// --- FSK: bit period taken from two falling edges -------------------------
int AudioWriter::encodeFsk(const std::uint8_t* data, unsigned /*startAddress*/, unsigned length) {
    int zero = 2, one = 4;
    digitalCycle(4, 2);

    switch (opt_.samplesPerBit) {
        case kS8_00: zero = 5; one = 11; break;
        case kS7_00: zero = 4; one = 10; break;
        case kS6_00: zero = 4; one = 8; break;
        case kS5_00: zero = 4; one = 6; break;
        case kS4_00: zero = 3; one = 5; break;
        case kS3_00: zero = 2; one = 4; break;
        case kS2_50: zero = 2; one = 3; break;
        default: break;
    }

    for (unsigned x = 0; x < length + 1; ++x) {
        const std::uint8_t mybyte = (x == 0) ? xorChecksum(data, static_cast<int>(length))
                                             : data[x - 1];
        std::uint8_t mask = 0x80;
        while (mask) {
            if (mybyte & mask)
                digitalCycle(one);
            else
                digitalCycle(zero);
            mask >>= 1;
        }
    }

    digitalCycle(8);
    digitalCycle(16);
    digitalCycle(24);
    digitalCycle(32);
    digitalCycle(40);
    digitalCycle(32);
    return 0;
}

// --- Shavings Slow: one cycle per 2 bits -----------------------------------
int AudioWriter::encodeShavingsSlow(const std::uint8_t* data, unsigned /*startAddress*/,
                                     unsigned length) {
    int pulses[4] = {0, 0, 0, 0};
    switch (opt_.samplesPerBit) {
        case kS4_00: pulses[0] = 5; pulses[1] = 7; pulses[2] = 9; pulses[3] = 11; break;
        case kS3_50: pulses[0] = 4; pulses[1] = 6; pulses[2] = 8; pulses[3] = 10; break;
        case kS3_00: pulses[0] = 3; pulses[1] = 5; pulses[2] = 7; pulses[3] = 9; break;
        case kS2_50: pulses[0] = 2; pulses[1] = 4; pulses[2] = 6; pulses[3] = 8; break;
        default: pulses[0] = 2; pulses[1] = 4; pulses[2] = 6; pulses[3] = 8; break;
    }

    digitalCycle(6);

    for (unsigned x = 0; x < length + 1; ++x) {
        const std::uint8_t byteValue = (x == 0) ? xorChecksum(data, static_cast<int>(length))
                                             : data[x - 1];
        // One cycle per 2-bit group, most significant first.  (An earlier
        // version emitted the third group as a raw 0..3 sample cycle instead
        // of looking it up in pulses[], which made every Slow tape unloadable.)
        digitalCycle(pulses[(byteValue & 0xc0) >> 6]);
        digitalCycle(pulses[(byteValue & 0x30) >> 4]);
        digitalCycle(pulses[(byteValue & 0x0c) >> 2]);
        digitalCycle(pulses[byteValue & 0x03]);
    }

    digitalCycle(pulses[3] + 1);
    digitalCycle(pulses[3] + 2);
    digitalCycle(pulses[3] + 3);
    digitalCycle(16);
    return 0;
}

// --- Shavings Delta: constant positive pulse, 2 bits per cycle -------------
int AudioWriter::encodeShavingsDelta(const std::uint8_t* data, unsigned /*startAddress*/,
                                      unsigned length) {
    digitalCycle(6, 2);
    digitalCycle(2, 2);
    if (opt_.samplesPerBit == kS1_75 || opt_.samplesPerBit == kS2_50)
        digitalCycle(2, 1);
    else
        digitalCycle(2, 2);

    for (unsigned x = 0; x < length; ++x) {
        std::uint8_t byteValue = data[x];
        int twoBits = 4;
        while (twoBits) {
            const int q = (byteValue & 0xc0) >> 6;
            switch (opt_.samplesPerBit) {
                case kS1_75: digitalCycle(p_[q], 1); break;
                case kS2_25: digitalCycle(p_[q], 2); break;
                case kS2_75: digitalCycle(p_[q] + 1, 2); break;
                case kS3_00: digitalCycle(p_[q] * 2 - 1, 2); break;
                case kS3_50: digitalCycle(p_[q] * 2, 2); break;
                default: break;
            }
            byteValue <<= 2;
            --twoBits;
        }
    }

    const int saved = opt_.waveform;
    if (opt_.waveform == 1) opt_.waveform = 0;

    digitalCycle(4, 8);
    digitalCycle(12);
    digitalCycle(14);
    digitalCycle(14);

    opt_.waveform = saved;
    return 0;
}

// --- Shavings "Raudo": the fast 2-bit cycle -------------------------------
int AudioWriter::encodeShavingsRaudo(const std::uint8_t* data, unsigned startAddress,
                                      unsigned length) {
    // The snapshot case skips the two byte patches that protect the loader
    // trampoline sitting at the top of RAM.
    const bool snp = (startAddress == 0xc000) && (length == 0x4000);

    digitalCycle(3, 2);
    digitalCycle(4);

    if (!snp && length > 0xfdbf - startAddress) length = 0xfdbf - startAddress;

    for (unsigned x = 0; x < length; ++x) {
        std::uint8_t byteValue;
        if (!snp) {
            if (startAddress + x == 0xfebf)
                byteValue = 0xdd;
            else if (startAddress + x == 0xfec0)
                byteValue = 0xe9;
            else
                byteValue = data[x];
        } else {
            byteValue = data[x];
        }

        switch (opt_.samplesPerBit) {
            case kS1_75:
                digitalCycle(((byteValue & 0xc0) >> 6) + 2);
                digitalCycle(((byteValue & 0x30) >> 4) + 2);
                digitalCycle(((byteValue & 0x0c) >> 2) + 2);
                digitalCycle((byteValue & 0x03) + 2);
                break;
            case kS2_25:
                digitalCycle(((byteValue & 0xc0) >> 6) + 3);
                digitalCycle(((byteValue & 0x30) >> 4) + 3);
                digitalCycle(((byteValue & 0x0c) >> 2) + 3);
                digitalCycle((byteValue & 0x03) + 3);
                break;
            case kS2_50:
                digitalCycle(((byteValue & 0xc0) >> 5) + 2);
                digitalCycle(((byteValue & 0x30) >> 3) + 2);
                digitalCycle(((byteValue & 0x0c) >> 1) + 2);
                digitalCycle(((byteValue & 0x03) * 2) + 2);
                break;
            case kS2_75:
                digitalCycle(((byteValue & 0xc0) >> 6) + 4);
                digitalCycle(((byteValue & 0x30) >> 4) + 4);
                digitalCycle(((byteValue & 0x0c) >> 2) + 4);
                digitalCycle((byteValue & 0x03) + 4);
                break;
            default: break;
        }
    }

    digitalCycle(3, 4);
    digitalCycle(12);
    digitalCycle(12);
    digitalCycle(16);
    return 0;
}

// --- Ultra: negative pulse = one bit, positive pulse = the next ------------
int AudioWriter::encodeUltra(const std::uint8_t* data, unsigned startAddress, unsigned length) {
    int zero = 2, one = 4;

    if ((opt_.samplesPerBit == kS1_50 || opt_.samplesPerBit == kS2_50) &&
        !((startAddress == 0xc000) && (length == 0x4000)))
        if (length > 0xfeff - startAddress) length = 0xfeff - startAddress;

    digitalCycle(4, 2);

    switch (opt_.samplesPerBit) {
        case kS1_50: zero = 1; one = 2; break;
        case kS2_00: zero = 1; one = 3; break;
        case kS2_50: zero = 2; one = 3; break;
        case kS3_00: zero = 2; one = 4; break;
        default: break;
    }

    for (unsigned x = 0; x < length; ++x) {
        digitalCycle((data[x] & 0x80) ? one : zero, (data[x] & 0x40) ? one : zero);
        digitalCycle((data[x] & 0x20) ? one : zero, (data[x] & 0x10) ? one : zero);
        digitalCycle((data[x] & 0x08) ? one : zero, (data[x] & 0x04) ? one : zero);
        digitalCycle((data[x] & 0x02) ? one : zero, (data[x] & 0x01) ? one : zero);
    }

    if (opt_.samplesPerBit == kS1_50 || opt_.samplesPerBit == kS2_50) {
        for (unsigned x = startAddress + length; x < (0xffu | (startAddress + length - 1)); ++x)
            for (int b = 0; b < 4; ++b) digitalCycle(2, 2);
    } else {
        digitalCycle(2, 4);
        digitalCycle(16);
    }

    digitalCycle(16);
    digitalCycle(16);
    return 0;
}

// --- Non Plus Ultra: 2 bits per pulse with a lookup table -----------------
int AudioWriter::encodeNonPlusUltra(const std::uint8_t* data, unsigned /*startAddress*/,
                                    unsigned length) {
    if (opt_.samplesPerBit == kS2_50)
        digitalCycle(4, 2);
    else
        digitalCycle(2, 1);

    for (unsigned x = 0; x < length + 1; ++x) {
        const std::uint8_t byteValue = (x == 0) ? xorChecksum(data, static_cast<int>(length))
                                             : data[x - 1];
        switch (opt_.samplesPerBit) {
            case kS1_25:
                digitalCycle(1 + ((byteValue & 0xc0) >> 6), 1 + ((byteValue & 0x30) >> 4));
                digitalCycle(1 + ((byteValue & 0x0c) >> 2), 1 + (byteValue & 0x03));
                break;
            case kS1_75:
                digitalCycle(2 + ((byteValue & 0xc0) >> 6), 2 + ((byteValue & 0x30) >> 4));
                digitalCycle(2 + ((byteValue & 0x0c) >> 2), 2 + (byteValue & 0x03));
                break;
            case kS2_00:
                digitalCycle(2 * (1 + ((byteValue & 0xc0) >> 6)) - 1,
                             2 * (1 + ((byteValue & 0x30) >> 4)) - 1);
                digitalCycle(2 * (1 + ((byteValue & 0x0c) >> 2)) - 1,
                             2 * (1 + (byteValue & 0x03)) - 1);
                break;
            case kS2_50:
                digitalCycle(2 * (1 + ((byteValue & 0xc0) >> 6)), 2 * (1 + ((byteValue & 0x30) >> 4)));
                digitalCycle(2 * (1 + ((byteValue & 0x0c) >> 2)), 2 * (1 + (byteValue & 0x03)));
                break;
            default: break;
        }
    }

    if (opt_.samplesPerBit >= kS2_00) {
        digitalCycle(10, 10);
        digitalCycle(10, 10);
        digitalCycle(10, 10);
        digitalCycle(10, 10);
    } else {
        digitalCycle(7, 7);
        digitalCycle(7, 7);
        digitalCycle(16);
        digitalCycle(16);
    }
    return 0;
}

// --- Fi: bit encoded by the high/low asymmetry of a fixed-length cycle ------
int AudioWriter::encodeFi(const std::uint8_t* data, unsigned /*startAddress*/, unsigned length) {
    int p0 = 4, p1 = 2;
    switch (opt_.samplesPerBit) {
        case kS8_00: p0 = 5; p1 = 3; break;
        case kS7_00: p0 = 5; p1 = 2; break;
        case kS6_00: p0 = 4; p1 = 2; break;
        case kS5_00: p0 = 3; p1 = 2; break;
        case kS4_00: p0 = 3; p1 = 1; break;
        case kS3_00: p0 = 2; p1 = 1; break;
        default: break;
    }

    digitalCycle(2, 3);

    for (unsigned x = 0; x < length + 1; ++x) {
        const std::uint8_t mybyte = (x == 0) ? xorChecksum(data, static_cast<int>(length))
                                             : data[x - 1];
        digitalCycle((mybyte & 0x80) ? p0 : p1, (mybyte & 0x80) ? p1 : p0);
        digitalCycle((mybyte & 0x40) ? p0 : p1, (mybyte & 0x40) ? p1 : p0);
        digitalCycle((mybyte & 0x20) ? p0 : p1, (mybyte & 0x20) ? p1 : p0);
        digitalCycle((mybyte & 0x10) ? p0 : p1, (mybyte & 0x10) ? p1 : p0);
        digitalCycle((mybyte & 0x08) ? p0 : p1, (mybyte & 0x08) ? p1 : p0);
        digitalCycle((mybyte & 0x04) ? p0 : p1, (mybyte & 0x04) ? p1 : p0);
        digitalCycle((mybyte & 0x02) ? p0 : p1, (mybyte & 0x02) ? p1 : p0);
        digitalCycle((mybyte & 0x01) ? p0 : p1, (mybyte & 0x01) ? p1 : p0);
    }

    digitalCycle(12);
    digitalCycle(14);
    digitalCycle(16);
    digitalCycle(20);
    digitalCycle(24);
    digitalCycle(32);
    digitalCycle(40);
    return 0;
}

// --- Fi Quadruple: 2 bits per cycle ----------------------------------------
int AudioWriter::encodeFiQ(const std::uint8_t* data, unsigned /*startAddress*/, unsigned length) {
    int p0a = 2, p0b = 2, p1a = 3, p1b = 3;

    switch (opt_.samplesPerBit) {
        case kS1_75: p0a = 1; p0b = 2; p1a = 1; p1b = 3; break;
        case kS2_00: p0a = 1; p0b = 2; p1a = 2; p1b = 3; break;
        case kS2_25: p0a = 1; p0b = 3; p1a = 2; p1b = 3; break;
        case kS2_50: p0a = 1; p0b = 3; p1a = 2; p1b = 4; break;
        case kS2_75: p0a = 2; p0b = 3; p1a = 2; p1b = 4; break;
        case kS3_00: p0a = 2; p0b = 3; p1a = 3; p1b = 4; break;
        case kS3_50: p0a = 2; p0b = 4; p1a = 3; p1b = 5; break;
        default: break;
    }

    digitalCycle(3, 2);

    for (unsigned x = 0; x < length; ++x) {
        std::uint8_t mybyte = data[x];
        unsigned bitsCount = 4;
        while (bitsCount--) {
            switch (mybyte & 0xc0) {
                case 0x00: digitalCycle(p0a, p0b); break;
                case 0x40: digitalCycle(p0b, p0a); break;
                case 0x80: digitalCycle(p1a, p1b); break;
                case 0xc0: digitalCycle(p1b, p1a); break;
                default: break;
            }
            mybyte <<= 2;
        }
    }

    digitalCycle(8);
    digitalCycle(16);
    digitalCycle(24);
    return 0;
}

// --- Manchester (IEEE 802.3) ------------------------------------------------
int AudioWriter::encodeManchester(const std::uint8_t* data, unsigned startAddress,
                                  unsigned length) {
    char lastBit = 0;
    int v = 0, c = 0, d = 1;

    if (opt_.samplesPerBit == kS3_00)
        c = 1;
    else if (opt_.samplesPerBit == kS4_00)
        d = 2;

    digitalCycle(4, 2 * d + c * 2);

    for (unsigned x = startAddress - 1; x < (0xffu | (startAddress + length - 1)) + 1 + 1; ++x) {
        std::uint8_t byteValue;
        if (x == startAddress - 1)
            byteValue = xorChecksum(data, (startAddress + length) < 0xff00
                                              ? static_cast<int>(length)
                                              : static_cast<int>(0xff00 - startAddress));
        else if (x < startAddress + length)
            byteValue = data[x - startAddress];
        else
            byteValue = 0;

        std::uint8_t mask = 0x80;
        while (mask) {
            const bool bit = (byteValue & mask) != 0;
            if (bit) {
                if (lastBit) {
                    digitalCycle(d * (1 + v + c), d * 1);
                    v = 0;
                } else {
                    v = 1;
                }
            } else {
                if (lastBit) {
                    digitalCycle(d * (1 + v + c), d * (2 + c));
                    v = 0;
                } else {
                    digitalCycle(d * 1, d * (1 + c));
                }
            }
            lastBit = bit ? 1 : 0;
            mask >>= 1;
        }
    }

    digitalCycle(d * (2 + c), d * (2 + c));
    digitalCycle(8);
    digitalCycle(16);
    return 0;
}

// --- Manchester with differential encoding ---------------------------------
int AudioWriter::encodeManchesterDif(const std::uint8_t* data, unsigned startAddress,
                                     unsigned length) {
    char lastBit = 0;
    int v = 0, c = 0, d = 1;
    int rising = 1;

    if (opt_.samplesPerBit == kS3_00)
        c = 1;
    else if (opt_.samplesPerBit == kS4_00)
        d = 2;

    digitalCycle(4, 2 * d + c * 2);
    lastBit = 0;

    for (unsigned x = startAddress - 1; x < (0xffu | (startAddress + length - 1)) + 1 + 1; ++x) {
        std::uint8_t byteValue;
        if (x == startAddress - 1)
            byteValue = xorChecksum(data, (startAddress + length) < 0xff00
                                              ? static_cast<int>(length)
                                              : static_cast<int>(0xff00 - startAddress));
        else if (x < startAddress + length)
            byteValue = data[x - startAddress];
        else
            byteValue = 0;

        std::uint8_t mask = 0x80;
        while (mask) {
            const bool bit = (byteValue & mask) != 0;
            if (bit != (lastBit != 0)) {
                if (rising)
                    digitalCycle(d * (1 + v), d * (1 + c));
                else
                    digitalCycle(d * (1 + v + c), d * 1);
                v = 0;
            } else {
                if (rising) {
                    v = 1;
                } else {
                    digitalCycle(d * (1 + v + c), d * (2 + c));
                    v = 0;
                }
                rising = !rising;
            }
            lastBit = bit ? 1 : 0;
            mask >>= 1;
        }
    }

    digitalCycle(d * (2 + c), d * (2 + c));
    digitalCycle(8);
    digitalCycle(16);
    return 0;
}

// --- "Escurrido": Shavings Delta with run-length merging -------------------
int AudioWriter::encodeEscurrido(const std::uint8_t* data, unsigned /*startAddress*/,
                                  unsigned length) {
    int incCero = 0, incData = 0, escalaData = 1, escalaCero = 1;

    if (opt_.samplesPerBit == kS2_50) escalaData = escalaCero = 2;
    if (opt_.samplesPerBit == kS2_25) escalaData = 2;
    if (opt_.samplesPerBit == kS1_50 || opt_.samplesPerBit == kS2_00 ||
        opt_.samplesPerBit == kS2_25)
        incCero = 1;
    if (opt_.samplesPerBit == kS2_00) incData = 1;

    int tAnterior = 2;

    digitalCycle(3 * escalaData, (1 + incCero) * escalaCero);
    digitalCycle(2 * escalaData, (1 + incCero) * escalaCero);
    digitalCycle(2 * escalaData, (1 + incCero) * escalaCero);

    int consecutive = 0;
    for (unsigned x = 0; x < length; ++x) {
        std::uint8_t byteValue = data[x];
        int twoBits = 4;
        while (twoBits) {
            const int t = byteValue >> 6;
            if (t == 0) {
                ++consecutive;
                if (consecutive > 2) {
                    digitalCycle(escalaData * (tAnterior + incData), escalaCero * (consecutive + incCero));
                    tAnterior = 4;
                    consecutive = 0;
                }
            } else {
                digitalCycle(escalaData * (tAnterior + incData), escalaCero * (1 + consecutive + incCero));
                tAnterior = t;
                consecutive = 0;
            }
            byteValue <<= 2;
            --twoBits;
        }
    }

    digitalCycle(escalaData * (tAnterior + incData), escalaCero * (1 + consecutive + incCero));
    digitalCycle(escalaData * (3 + incData), escalaCero * (1 + incCero));
    digitalCycle(escalaData * (6 + incData), escalaCero * (1 + incCero));
    digitalCycle(escalaData * (8 + incData), escalaCero * (1 + incCero));
    return 0;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
void AudioWriter::encodeBlock(const std::uint8_t* data, unsigned startAddress, unsigned length) {
    switch (opt_.method) {
        case kRom: encodeRom(data, startAddress, length); break;
        case kMilks: encodeThrwMilks(data, startAddress, length); break;
        case kFsk: encodeFsk(data, startAddress, length); break;
        case kShavingsSlow: encodeShavingsSlow(data, startAddress, length); break;
        case kShavingsDelta: encodeShavingsDelta(data, startAddress, length); break;
        case kShavingsRaudo: encodeShavingsRaudo(data, startAddress, length); break;
        case kUltra: encodeUltra(data, startAddress, length); break;
        case kNpu: encodeNonPlusUltra(data, startAddress, length); break;
        case kFi: encodeFi(data, startAddress, length); break;
        case kFiQ: encodeFiQ(data, startAddress, length); break;
        case kManchester: encodeManchester(data, startAddress, length); break;
        case kManchesterDif: encodeManchesterDif(data, startAddress, length); break;
        case kEscurrido: encodeEscurrido(data, startAddress, length); break;
        // kRayo renders through its own signal builder, not this switch, and
        // Methods 13-19 were removed.  encodeEscurrido is the
        // slowest remaining encoder, so it is the safe last resort.
        default: encodeEscurrido(data, startAddress, length); break;
    }
}

void AudioWriter::renderCycles(const std::vector<std::pair<std::uint16_t, std::uint16_t>>& cycles) {
    const int savedWaveform = opt_.waveform;
    opt_.waveform = kSquare;
    db_ = 115;
    for (const auto& c : cycles) digitalCycle(c.first, c.second);
    opt_.waveform = savedWaveform;
}

void AudioWriter::renderEncodedBlock(const std::uint8_t* data, unsigned short startAddress,
                              unsigned short length) {
    const int savedWaveform = opt_.waveform;
    if (opt_.waveform == kRamp || opt_.waveform == kZeroContinuous || opt_.waveform == kDelta)
        opt_.waveform = kSquare;

    if (snapshotBlocks_)
        pilotDigital(1305);
    else
        pilotDigital(305);
    digitalCycle(kTP1, kTP2);
    digitalCycle(kTS1, kTS2);

    opt_.waveform = savedWaveform;
    encodeBlock(data, startAddress, length);
}

}  // namespace k7zx
