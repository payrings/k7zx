// k7zx 5.0 - modern C++ port
//
// Rayo: a turbo loader added by this port (not part of k7zx 4.3).
//
// Same signal family as Shavings Raudo -- two bits per full wave cycle, cycle
// lengths Pmin..Pmin+3 samples (Pmin 3 = "2.25", Pmin 4 = "2.75"), decoded
// with Raudo's `in l,(c) / jp (hl)` dispatch and the R register as the clock
// -- plus what Raudo lacks:
//
//   * a checksum and an end-address check: a bad load stops with
//     "R Tape loading error" instead of running corrupt code;
//   * the polarity is learnt from the sync, so an inverted signal loads;
//   * the port is read as $FFFE, so a key held during loading does no harm;
//   * the 2-bit values are mapped to cycle lengths by frequency, per tape;
//   * optional LZ compression, expanded in place by the loader (see lz.h);
//   * every pulse is timed as explicit low/high half-cycles and rendered square,
//     which a DAC's reconstruction filter reproduces more faithfully than a
//     shaped waveform.
//
// The Z80 code is src/core/asm/rayo.asm; tools/rayo/build_loader.py turns it
// into rayo_loader.inc.  The decode table and presets come from the timing
// model below, which was measured in tools/zxload against 48K and 128K timing
// and through a model of a real playback chain.
#ifndef K7ZX_RAYO_H
#define K7ZX_RAYO_H

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace k7zx::rayo {

/// Lowest address the loader occupies while it runs; data must end below it.
std::uint16_t loaderBase();

struct Request {
    int sampleRate = 48000;        ///< 44100 or 48000
    int pmin = 3;                  ///< shortest cycle in samples: 3 ("2.25") or 4 ("2.75")
    const std::uint8_t* data = nullptr;
    unsigned dest = 0;             ///< where the data belongs
    unsigned length = 0;
    bool compress = true;
    unsigned usr = 0;              ///< JP here afterwards; 0 = RET to BASIC
    bool snapshot = false;         ///< leave interrupts off (the restore stub decides)
    unsigned clear = 0xfd3f;       ///< CLEAR in the BASIC line (kept below the loader)
};

struct Program {
    std::vector<std::uint8_t> line;   ///< the BASIC line: number, length, body, CR
    /// The turbo section as (low, high) half-cycle lengths in samples.
    std::vector<std::pair<std::uint16_t, std::uint16_t>> cycles;
    bool compressed = false;
    std::size_t sentBytes = 0;        ///< bytes on the turbo section (with checksum)
    unsigned loadAddress = 0;         ///< where the sent bytes land
    std::string warnings;
};

/// Build the loader program and the turbo stream.  Returns false and fills
/// `error` if the request cannot be met.
bool build(const Request& rq, Program& out, std::string& error);

/// The table R -> 2-bit value the loader uses (index = R, 0..127), and the
/// presets.  Exposed for the tests.
struct Tables {
    std::array<std::uint8_t, 128> table{};
    std::uint8_t preset = 0, syncPreset = 0;
    std::array<int, 4> slotValue{0, 1, 2, 3};  ///< cycle slot (shortest first) -> value
};
Tables tables(int sampleRate, int pmin, const std::array<int, 4>& slotValue);

}  // namespace k7zx::rayo

#endif  // K7ZX_RAYO_H
