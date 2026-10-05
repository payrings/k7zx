// k7zx 5.0 - modern C++ port
//
// The loader synthesizer: builds a short BASIC program whose payload is a
// hand-assembled Z80 routine that reads the tape at the chosen encoding, then
// hands the resulting memory image to AudioWriter.  Ported from ZXCODE.CPP.
#ifndef K7ZX_ZXCODE_H
#define K7ZX_ZXCODE_H

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "defs.h"
#include "loader.h"
#include "zxfiles.h"
#include "zxwav.h"

namespace k7zx {

/// Options that steer the high-speed converter but do not affect the raw
/// waveform samples directly.
struct LoaderOptions {
    Scheme scheme = kManyBlocks;  ///< blockScheme
    bool generateLoader = true;       ///< emit the synthetic BASIC loader
    bool controlChecksum = true;     ///< loader validates the tape checksum
    bool antiKolmogorov = false;     ///< statistical optimisation of Delta timings
    bool compress = true;            ///< LZ compression (Rayo only)
    unsigned clearAddress = 65344;       ///< 0xff40
    unsigned usrAddress = 0;              ///< entry point
};

/// Assembles the synthetic loader and renders it.
///
/// Holds all the state the original kept in file-scope globals: the patched
/// loader routine (`loaderRoutine_`), the generated TAP block (`BasicTap`), the
/// multi-block dispatcher and the assembled memory image.
class HighSpeedConverter {
public:
    HighSpeedConverter(TapeImage& image, const AudioOptions& audio, const LoaderOptions& loader);

    /// Write the WAV (or TZX when audio.emulate is set) to `outPath`.
    /// Returns false and fills errorMessage() on failure.
    bool convert(const std::string& outPath);

    const std::string& errorMessage() const { return error_; }
    const std::string& warnings() const { return warnings_; }
    double duration() const { return duration_; }
    const AudioWriter& audio() const { return *writer_; }

    LoaderOptions& options() { return loaderOpts_; }
    const LoaderOptions& options() const { return loaderOpts_; }

private:
    // --- loader routine preparation -----------------------------------------
    void copyRoutine(Method m);
    void copyExtraRoutine(ExtraRoutine r);
    void statisticalAnalysis();
    void prepareLoaderRoutine();  ///< prepareLoaderRoutine

    // --- table builders (write into the patched routine) --------------------
    void makeTable(int x, int d00, int d01, int d10, int d11);
    void makeTableRaudo(int x, int d00, int d01, int d10, int d11);
    void makeTableSlow(int x, int d00, int d01, int d10, int d11);
    void makeTableEscu2(int x, int d01, int d10, int d11, int d00);
    void makeTableEscu1(int x, int d01, int d10, int d11, int d00);
    void makeTableNpuP(int x, int d00, int d01, int d10, int d11);

    // --- memory assembly -----------------------------------------------------
    void applyPokes();
    /// Redirect the routine's preamble jump at the multi-block dispatcher.
    void pointPreambleAtDispatcher(std::uint8_t* data);
    void prepareBasicBlock(int index);
    unsigned joinBlocks();
    void buildMultiblock();
    void relocate128Code();

    // --- synthetic BASIC program --------------------------------------------
    void startBasic();
    void finishBasic();
    void changeLoadCode(std::uint8_t* bc, std::size_t n);

    void convertSnapshot48K();
    void convertSnapshot128K();
    void convertSingleBlock();
    void convertMultiBlock();
    void convertOriginalLoader();
    /// Rayo (added by this port) builds its own loader: see rayo.h.
    void convertRayo();
    /// The two standard-speed blocks (header + program) for a ready-made line.
    void writeBasicLine(const std::vector<std::uint8_t>& line);
    /// The 10-byte name for the loader's tape header.
    std::string tapeName() const;

    void fail(const std::string& msg) { error_ += msg; }

    TapeImage& image_;
    AudioOptions audioOpts_;
    LoaderOptions loaderOpts_;
    std::unique_ptr<AudioWriter> writer_;
    std::string error_;
    std::string warnings_;
    double duration_ = -1;

      // Patched copy of the selected Z80 loader.
      std::array<std::uint8_t, 512> loaderRoutine_{};
    int maxPos_ = 0;
    int jpPos_ = 0;
    int eiPos_ = 0;
    int routineLength_ = 0;

    // The synthetic TAP block being assembled.
    std::array<std::uint8_t, 4096> basicTap_{};
    std::size_t lineOffset_ = 0;   ///< start of the BASIC line's data field
    unsigned longLineLength_ = 0;  ///< declared length of that line

    // Multi-block dispatcher.  The original sized this at 70 bytes but then
    // strcpy'd the 74-byte Manchester table into it; 80 is enough for 7 blocks.
    std::array<std::uint8_t, 80> multiloadCode_{};

    // Frequency histogram used by the Delta timing optimiser.
    struct Histo {
        double count = 0;
        int symbol = 0;
    };
    std::array<Histo, 5> histo_{};
    int deltaDurations_[5] = {1, 2, 3, 4, 0};

    unsigned newStart_ = 0;
    unsigned newEnd_ = 0;
    unsigned max_ = 0;
    unsigned totalBlocks_ = 0;
};

/// The "normal" tap/tzx -> wav converter (convierteNORMAL, TAP2WAV, TZX2WAV).
///
/// Replays a standard-speed tape verbatim: 2168 T-state pilot pulses, a
/// 667/735 sync pair, then 855/1710 T-state data bits.  Snapshots and
/// turbo blocks are not supported here, exactly as in the original.
class NormalConverter {
public:
    NormalConverter(const AudioOptions& audio);

    /// Convert `inPath` (a .tap or .tzx file) to `outPath`.
    bool convert(const std::string& inPath, const std::string& outPath);

    const std::string& errorMessage() const { return error_; }
    const std::string& warnings() const { return warnings_; }
    double duration() const { return duration_; }

private:
    void convertTap();
    void convertTzx();
    void fail(const std::string& msg) { error_ += msg; }
    void warn(const std::string& msg) { warnings_ += msg; }

    AudioOptions audioOpts_;
    std::unique_ptr<AudioWriter> writer_;
    std::vector<std::uint8_t> filebuf_;
    std::string error_;
    std::string warnings_;
    double duration_ = -1;
};

}  // namespace k7zx

#endif  // K7ZX_ZXCODE_H
