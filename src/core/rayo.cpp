// k7zx 5.0 - modern C++ port
//
// Rayo loader assembly and encoder.  See rayo.h.
#include "rayo.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "lz.h"
#include "rayo_loader.inc"

namespace k7zx::rayo {
namespace {

// --- the timing model --------------------------------------------------------
// The loader reads R at the end of every cycle; each 16 T poll adds 3, so
//     R ~= preset + kC0 + kSlope * T_cycle.
// kC0 was measured in tools/zxload with exact edges and then shifted so the
// windows sit between 48K (3.5 MHz) and 128K (3.5469 MHz) timing; with it no
// symbol errors were seen at either through the playback-chain model (README,
// step 14).
constexpr double kCpu = 3500000.0;
constexpr double kSlope = 3.0 / 16.0;
constexpr double kC0 = -19.0;
constexpr int kDataLo = 44;        // R of the shortest data cycle
constexpr int kSyncTop = kDataLo - 12;  // R 4..kSyncTop decode to the sync sentinel
constexpr int kSyncR = 18;         // where the sync cycle aims
constexpr double kSyncK = -57.7;   // R offset of the sync measurement (measured)

// --- the signal ----------------------------------------------------------------
constexpr int kPilotCycles = 400;  // 8+8 samples each; also covers BASIC's start-up
constexpr int kSyncShort = 2, kSyncLong = 8;
constexpr int kTailCycles = 6;

double samplePeriod(int rate) { return kCpu / rate; }

int rOf(double samples, int preset, int rate) {
    const double r = preset + kC0 + kSlope * samples * samplePeriod(rate);
    return static_cast<int>(std::lround(r)) & 0x7f;
}

void poke(std::uint8_t* top, unsigned addr, unsigned v) { top[addr - kPage] = static_cast<std::uint8_t>(v); }
void poke16(std::uint8_t* top, unsigned addr, unsigned v) {
    poke(top, addr, v & 0xff);
    poke(top, addr + 1, (v >> 8) & 0xff);
}

}  // namespace

std::uint16_t loaderBase() { return k_area0; }

Tables tables(int rate, int pmin, const std::array<int, 4>& slotValue) {
    Tables t;
    t.slotValue = slotValue;
    const double spt = samplePeriod(rate);
    t.preset = static_cast<std::uint8_t>(
        static_cast<int>(std::lround(kDataLo - kC0 - kSlope * pmin * spt)) & 0x7f);
    t.syncPreset = static_cast<std::uint8_t>(
        static_cast<int>(std::lround(kSyncR - kSyncK - kSlope * kSyncLong * spt)) & 0x7f);
    t.table.fill(0x80);  // terminator / invalid
    for (int R = 4; R < 128; ++R) {
        if (R <= kSyncTop) { t.table[R] = 1; continue; }
        const double T = (((R - t.preset) % 128 + 128) % 128 - kC0) / kSlope;
        const double x = T / spt;  // cycle length in samples
        // Inner boundaries half way between symbols; the outer slots reach a
        // whole sample out, which absorbs a few % of tape speed error.
        if (x >= pmin - 1.0 && x < pmin + 3 + 1.0) {
            const int n = std::clamp(static_cast<int>(std::lround(x)), pmin, pmin + 3);
            t.table[R] = static_cast<std::uint8_t>(slotValue[n - pmin]);
        }
    }
    return t;
}

bool build(const Request& rq, Program& out, std::string& error) {
    out = Program{};
    if (rq.sampleRate != 44100 && rq.sampleRate != 48000) {
        error = "Rayo needs 44100 or 48000 Hz";
        return false;
    }
    if (rq.pmin != 3 && rq.pmin != 4) {
        error = "Rayo runs at 2.25 or 2.75 samples per bit";
        return false;
    }
    if (rq.length == 0 || !rq.data) {
        error = "nothing to load";
        return false;
    }
    if (rq.dest < 0x4000 || rq.dest + rq.length > k_area0) {
        error = "Rayo data must lie between 16384 and the loader at " + std::to_string(k_area0);
        return false;
    }

    // --- what goes on the tape ---------------------------------------------------
    std::vector<std::uint8_t> sent(rq.data, rq.data + rq.length);
    unsigned load = rq.dest;
    if (rq.compress) {
        std::vector<std::uint8_t> c = lz::compress(rq.data, rq.length);
        const int margin = lz::inPlaceMargin(rq.data, rq.length, c);
        // The stream ends `margin` bytes past the data, and its checksum byte is
        // stored one further on; both must stay below the decompressor.
        const long end = static_cast<long>(rq.dest) + rq.length + margin + 1;
        if (margin < 0) {
            out.warnings += "Rayo: compression failed its self-check; sent uncompressed\r\n";
        } else if (c.size() + 1 >= rq.length) {
            out.warnings += "Rayo: the data does not compress; sent uncompressed\r\n";
        } else if (end > k_dzxstart) {
            out.warnings += "Rayo: no room above the data for in-place expansion; sent uncompressed\r\n";
        } else {
            load = rq.dest + rq.length + static_cast<unsigned>(margin) - static_cast<unsigned>(c.size());
            sent = std::move(c);
            out.compressed = true;
        }
    }
    std::uint8_t sum = 0;
    for (std::uint8_t b : sent) sum ^= b;
    sent.push_back(sum);  // stored too: the loader checks the XOR of everything is 0

    // --- map the commonest 2-bit value to the shortest cycle -------------------
    std::array<long, 4> count{};
    for (std::uint8_t b : sent)
        for (int sh = 6; sh >= 0; sh -= 2) ++count[(b >> sh) & 3];
    std::array<int, 4> slotValue{0, 1, 2, 3};
    std::stable_sort(slotValue.begin(), slotValue.end(),
                     [&](int a, int b) { return count[a] > count[b]; });
    std::array<int, 4> valueSlot{};
    for (int s = 0; s < 4; ++s) valueSlot[slotValue[s]] = s;
    const Tables tb = tables(rq.sampleRate, rq.pmin, slotValue);
    const int term = rq.pmin + 5;
    if (tb.table[rOf(term, tb.preset, rq.sampleRate)] != 0x80) {
        error = "internal: the terminator does not decode as one";
        return false;
    }

    // --- patch the loader image ---------------------------------------------------
    std::array<std::uint8_t, sizeof kTop> top{};
    std::memcpy(top.data(), kTop, sizeof kTop);
    const unsigned endAddr = (load + static_cast<unsigned>(sent.size())) & 0xffff;
    poke16(top.data(), k_segtab, load);
    poke16(top.data(), k_segtab + 2, endAddr);
    poke(top.data(), k_segtab + 4, 0);
    poke(top.data(), k_segtab + 9, 0xff);  // the next entry ends the table
    poke(top.data(), k_presetn + 1, tb.preset);
    poke(top.data(), k_presetf + 1, tb.preset);
    poke(top.data(), k_presets + 1, tb.syncPreset);
    poke(top.data(), k_decomp + 1, out.compressed ? 1 : 0);
    poke16(top.data(), k_dsrc + 1, load);
    poke16(top.data(), k_ddst + 1, rq.dest);
    poke(top.data(), k_eiop, rq.snapshot ? 0x00 : 0xfb);  // NOP / EI
    if (rq.usr) {
        poke(top.data(), k_jpop, 0xc3);
        poke16(top.data(), k_jpop + 1, rq.usr);
    } else {
        poke(top.data(), k_jpop, 0xc9);  // RET
    }

    // --- the BASIC line ---------------------------------------------------------------
    // CLEAR VAL "nnnnn" : RANDOMIZE USR VAL "23781" : STR$   -- 22 bytes, so the
    // boot code that follows starts at 23755 + 4 + 22 = 23781.
    const unsigned clear = std::min(rq.clear, static_cast<unsigned>(k_area0 - 1));
    char text[32];
    std::snprintf(text, sizeof text, "\xfd\xb0\"%05u\":\xf9\xc0\xb0\"23781\":\xea", clear);
    std::vector<std::uint8_t> body(text, text + 22);
    body.insert(body.end(), kBoot, kBoot + sizeof kBoot);
    // the loader, in chunks: address (2), count (1), bytes
    struct Area { unsigned lo, hi; };
    const Area areas[] = {{k_area0, out.compressed ? k_area0end : k_dzxstart},
                          {k_area1, k_area1end}, {k_area2, k_area2end},
                          {k_area3, k_area3end}, {k_area4, k_area4end}};
    for (const Area& a : areas) {
        for (unsigned lo = a.lo; lo < a.hi;) {
            const unsigned n = std::min(255u, a.hi - lo);
            body.push_back(static_cast<std::uint8_t>(lo & 0xff));
            body.push_back(static_cast<std::uint8_t>(lo >> 8));
            body.push_back(static_cast<std::uint8_t>(n));
            body.insert(body.end(), top.begin() + (lo - kPage), top.begin() + (lo - kPage + n));
            lo += n;
        }
    }
    body.push_back(0);
    body.push_back(0);
    // the table, run-length coded from index 4
    for (int i = 4; i < 128;) {
        int j = i;
        while (j < 128 && tb.table[j] == tb.table[i] && j - i < 255) ++j;
        body.push_back(static_cast<std::uint8_t>(j - i));
        body.push_back(tb.table[i]);
        i = j;
    }
    body.push_back(0);
    body.push_back(0x0d);
    out.line = {0x00, 0x00, static_cast<std::uint8_t>(body.size() & 0xff),
                static_cast<std::uint8_t>(body.size() >> 8)};
    out.line.insert(out.line.end(), body.begin(), body.end());

    // --- the turbo section --------------------------------------------------------------
    auto cycle = [&](int p) {
        out.cycles.emplace_back(static_cast<std::uint16_t>(p / 2), static_cast<std::uint16_t>(p - p / 2));
    };
    for (int i = 0; i < kPilotCycles; ++i) out.cycles.emplace_back(8, 8);
    out.cycles.emplace_back(kSyncShort, kSyncLong);
    for (std::uint8_t b : sent)
        for (int sh = 6; sh >= 0; sh -= 2) cycle(rq.pmin + valueSlot[(b >> sh) & 3]);
    cycle(term);
    for (int i = 0; i < kTailCycles; ++i) out.cycles.emplace_back(8, 8);
    out.sentBytes = sent.size();
    out.loadAddress = load;
    return true;
}

}  // namespace k7zx::rayo
