// k7zx 5.0 - modern C++ port
//
// The signal generator.  Ported from zxwav.cpp: it turns abstract tape events
// (pilot pulses, sync pulses, data bits) into a PCM stream that a ZX Spectrum
// ULA can read, and wraps that stream in either a RIFF/WAVE container or a
// TZX direct-recording block.
//
// The original wrote through a global FILE*, a fixed 512-entry scratch
// waveform buffer and a pile of file-scope flags.  All of that now lives in
// AudioWriter, so two conversions can run without stepping on each other and
// the scratch buffer grows instead of overflowing.
#ifndef K7ZX_ZXWAV_H
#define K7ZX_ZXWAV_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "defs.h"

namespace k7zx {

/// All the knobs that affect the rendered waveform.
struct AudioOptions {
    int sampleRate = 48000;    ///< Sample rate the WAV is written at (the original called it FqMuestreo)
    int method = kMilks;      ///< Method, as int so it can come from config
    int samplesPerBit = kS2_75;
    int waveform = kCubic;    ///< Waveform
    bool stereo = false;      ///< 16-bit stereo (the original's CD)
    bool invert = false;      ///< Invert polarity (the original's sign == -1)
    bool invertRight = false; ///< rightSign == -1
    bool finalTone = false;    ///< Extra half-second of tone at the end (the original called it tono_final)
    bool emulate = false;     ///< emit a TZX instead of a WAV
    bool accelerateBasic = false;

    /// Effective sample rate actually written into the file header.
    int effectiveRate() const { return sampleRate; }
};

/// Renders tape events to a WAV or TZX file.
///
/// The pulse timing tables below are the *same* numbers as the original: they
/// are expressed in waveform-scrubbing steps rather than T-states, and are
/// used as the WAV's declared sample rate.
class AudioWriter {
public:
    explicit AudioWriter(const AudioOptions& opt);
    ~AudioWriter();
    AudioWriter(const AudioWriter&) = delete;
    AudioWriter& operator=(const AudioWriter&) = delete;

    /// Open `path` and write the container header.
    bool open(const std::string& path);

    /// Write any trailing tone, patch the container header and close.
    bool close();

    const std::string& error() const { return error_; }

    /// Duration of the audio written so far, in seconds.
    double duration() const { return duration_; }

    // ---- low level waveform primitives -------------------------------------

    /// A square/digital cycle: `t` sub-samples low then high.
    int digitalCycle(int t);
    /// A digital cycle with asymmetric halves.
    int digitalCycle(int low, int high);

    /// A smoothed (cubic) cycle with an explicit low/high duration.
    int analogCycle(double p1, double p2);
    /// A smoothed cycle of total duration `p`, split evenly.
    int analogCycle(double p);

    // ---- tape event primitives ------------------------------------------------

    void pilotDigital(int n);
    void pureTone(double pilot, int n);
    void sync(double sync1, double sync2);
    void sequence(const std::uint16_t* pulses, int n);
    void pureData(double zero, double one, const std::uint8_t* data, int n, int usedBits);
    void pause(double p, int milliseconds);
    void directRecording(unsigned shortTstates, const std::uint8_t* data, int n);

    /// Emit one standard-speed loader block (pilot + sync + data + checksum).
    ///
    /// `accelerated` selects the faster BASIC-load timings.  It must be FALSE
    /// for the BASIC header: that block is read by the Spectrum's own ROM, which
    /// expects standard-speed pulses.  Only the code block, which the turbo
    /// loader reads, may be sped up.  k7zx 4.3 applied the speedup to both,
    /// which is why the BASIC header sounded 14% sharp.
    void loaderBlock(const std::uint8_t* data, unsigned length, bool accelerated);

    /// The high-speed "shiver" writer: pilot/sync followed by one encoded
    /// block using the currently selected method.
    void renderEncodedBlock(const std::uint8_t* data, unsigned short startAddress, unsigned short length);
    /// Rayo's turbo section (added by this port): (low, high) half-cycle
    /// lengths in samples, always rendered as square waves -- a square edge is
    /// timed most accurately after a DAC's reconstruction filter.
    void renderCycles(const std::vector<std::pair<std::uint16_t, std::uint16_t>>& cycles);

    /// Render one block with the currently selected method.
    void encodeBlock(const std::uint8_t* data, unsigned startAddress, unsigned length);

    /// Shavings Delta encodes each 2-bit group with one of four pulse widths
    /// chosen by a statistical analysis of the payload.  `p` holds those four
    /// durations plus a terminator.
    void setDeltaDurations(const int p[5]) {
        for (int i = 0; i < 5; ++i) p_[i] = p[i];
    }

    /// True when the loaded image is made of explicit snapshot pages, which
    /// changes the pilot tone length.
    void setSnapshotBlocks(bool v) { snapshotBlocks_ = v; }


private:
    // Per-method encoders (EncodeXXX in the original).
    int encodeRom(const std::uint8_t* data, unsigned startAddress, unsigned length);
    int encodeThrwMilks(const std::uint8_t* data, unsigned startAddress, unsigned length);
    int encodeFsk(const std::uint8_t* data, unsigned startAddress, unsigned length);
    int encodeShavingsSlow(const std::uint8_t* data, unsigned startAddress, unsigned length);
    int encodeShavingsDelta(const std::uint8_t* data, unsigned startAddress, unsigned length);
    int encodeShavingsRaudo(const std::uint8_t* data, unsigned startAddress, unsigned length);
    int encodeUltra(const std::uint8_t* data, unsigned startAddress, unsigned length);
    int encodeNonPlusUltra(const std::uint8_t* data, unsigned startAddress, unsigned length);
    int encodeFi(const std::uint8_t* data, unsigned startAddress, unsigned length);
    int encodeFiQ(const std::uint8_t* data, unsigned startAddress, unsigned length);
    int encodeManchester(const std::uint8_t* data, unsigned startAddress, unsigned length);
    int encodeManchesterDif(const std::uint8_t* data, unsigned startAddress, unsigned length);
    int encodeEscurrido(const std::uint8_t* data, unsigned startAddress, unsigned length);

    // Standard-speed (tape2wav style) loaders.
    void loaderNormal(std::uint8_t* data, unsigned length);
    void loaderAccelerated(std::uint8_t* data, unsigned length);

    /// Convert an int waveform through sign/scale and append to the output.
    int emit(const int* y, int n);
    void buildWaveTable(int* f, int t);

    void writeContainerHeader();
    void patchContainerHeader();

    AudioOptions opt_;

    std::FILE* f_ = nullptr;
    std::string error_;

    std::vector<int> scratch_;   ///< waveform scratch, grows on demand
    /// Interleaved 16-bit stereo frames (L, R, L, R, ...).
    std::vector<std::int16_t> stereoInterleaved_;
    std::vector<std::uint8_t> mono_;

    // TZX direct-recording state
    std::vector<std::uint8_t> tzxHeader_;
    std::size_t tzxDataOffset_ = 0;
    std::uint32_t tzxBytes_ = 0;
    int nBit_ = 1;               ///< running bit accumulator, always starts at 1

    // WAV state
    std::uint32_t dataBytes_ = 0;
    std::uint16_t blockAlign_ = 1;

    double db_ = 0x75;           ///< current amplitude envelope
    double phase_ = 0;           ///< cross-fade offset for analog cycles
    double duration_ = -1;

    /// Per-bit durations used by Shavings Delta; filled by the converter's
    /// statistical analysis (the global `p[]` in the original).
    int p_[5] = {1, 2, 3, 4, 0};

    double pulseLengths_[5] = {54.8, 18.6, 21.6, 43.3, 0};
    bool snapshotBlocks_ = false;  ///< snapshot_blocks

};

}  // namespace k7zx

#endif  // K7ZX_ZXWAV_H
