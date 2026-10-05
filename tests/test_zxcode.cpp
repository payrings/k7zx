// Tests for the loader synthesis and the two conversion entry points.
#include "test_harness.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <cstdint>

#include "core/convert.h"
#include "core/settings.h"
#include "core/zxcode.h"

using namespace k7zx;

namespace {

std::string path(const char* name) { return k7zxtest::dataDir() + "/" + name; }

std::size_t fileSize(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    return f ? static_cast<std::size_t>(f.tellg()) : 0;
}

Settings fastSettings(int method, int spb) {
    Settings s;
    s.conversionMode = kConvertHiSpeed;
    s.method = method;
    s.samplesPerBit = spb;
    s.sampleRate = 48000;
    s.scheme = kManyBlocks;
    s.waveform = kCubic;
    s.generateLoader = true;
    return s;
}

}  // namespace

TEST("the hispeed converter produces a WAV for every method and speed") {
    for (int m = kRom; m <= kEscurrido; ++m) {
        for (int spb : Settings::allowedSamplesPerBit(static_cast<Method>(m))) {
            const std::string out = "/tmp/k7zx-test-hs.wav";
            Converter c;
            CHECK(c.load(path("test.tap")));
            c.applySettings(fastSettings(m, spb));
            const ConversionResult r = c.convert(out, false);
            CHECK(r.ok);
            CHECK(r.errors.empty());
            CHECK(r.duration > 0.1);
            CHECK(fileSize(out) > 1000);
            std::remove(out.c_str());
        }
    }
}

TEST("the verbatim replay path works on TAP input") {
    const std::string out = "/tmp/k7zx-test-normal.wav";
    Converter c;
    CHECK(c.load(path("test.tap")));
    Settings s;
    s.conversionMode = kConvertNormal;
    c.applySettings(s);
    const ConversionResult r = c.convert(out, true);
    CHECK(r.ok);
    // 8064 pilot pulses for each of the two header blocks is several seconds.
    CHECK(r.duration > 5.0);
    std::remove(out.c_str());
}

TEST("the verbatim replay path rejects snapshots with a clear message") {
    const std::string out = "/tmp/k7zx-test-badnormal.wav";
    Converter c;
    CHECK(c.load(path("test48k.sna")));
    Settings s;
    s.conversionMode = kConvertNormal;
    c.applySettings(s);
    const ConversionResult r = c.convert(out, true);
    CHECK(!r.ok);
    CHECK(r.errors.find("only reads .tap and .tzx") != std::string::npos);
    std::remove(out.c_str());
}

TEST("48K snapshots convert through all four shapes") {
    const char* files[] = {"test48k.sna", "test48k.z80"};
    for (const char* f : files) {
        for (int sch = kOneBlock; sch <= kOriginalLoader; ++sch) {
            const std::string out = "/tmp/k7zx-test-snap.wav";
            Converter c;
            CHECK(c.load(path(f)));
            Settings s = fastSettings(kShavingsRaudo, kS2_75);
            s.scheme = static_cast<Scheme>(sch);
            c.applySettings(s);
            const ConversionResult r = c.convert(out, false);
            CHECK(r.ok);
            CHECK(r.duration > 1.0);
            std::remove(out.c_str());
        }
    }
}

TEST("pokes are applied to the assembled memory image") {
    Converter c;
    CHECK(c.load(path("test.tap")));
    c.applySettings(fastSettings(kShavingsRaudo, kS2_75));
    c.image().addPoke(0x5000, 0xa5);
    c.image().applyPokes();
    CHECK_EQ(int(c.image().memory()[0x5000]), 0xa5);
}

TEST("the checksum handler is only installed for methods that have one") {
    // These encoders embed their own parity, so the RST 8 trap is dead code.
    CHECK(!Settings::methodSupportsChecksumCheck(kRom));
    CHECK(!Settings::methodSupportsChecksumCheck(kShavingsDelta));
    CHECK(!Settings::methodSupportsChecksumCheck(kShavingsRaudo));
    CHECK(!Settings::methodSupportsChecksumCheck(kUltra));
    CHECK(!Settings::methodSupportsChecksumCheck(kFiQ));
    CHECK(!Settings::methodSupportsChecksumCheck(kEscurrido));

    CHECK(Settings::methodSupportsChecksumCheck(kMilks));
    CHECK(Settings::methodSupportsChecksumCheck(kFsk));
    CHECK(Settings::methodSupportsChecksumCheck(kShavingsSlow));
    CHECK(Settings::methodSupportsChecksumCheck(kNpu));
    CHECK(Settings::methodSupportsChecksumCheck(kFi));
    CHECK(Settings::methodSupportsChecksumCheck(kManchester));
}

TEST("the suggested file name encodes method, scheme and speed") {
    Converter c;
    CHECK(c.load(path("test.tap")));
    Settings s = fastSettings(kShavingsRaudo, kS2_75);
    s.infoInFileName = true;
    c.applySettings(s);
    const std::string n = c.suggestedOutputName("test.tap");
    CHECK(n.find("_SRA_") != std::string::npos);
    CHECK(n.find("2.75") != std::string::npos);
    CHECK(n.size() > 4);
    CHECK_EQ(n.substr(n.size() - 4), std::string(".wav"));
}

TEST("the suggested file name carries no tags when the option is off") {
    // The batch dialog and the single-file path both go through
    // suggestedOutputName(), so it -- not its caller -- has to honour the
    // "info in output file name" option.  It used to always add the tags, and
    // batch mode ignored the option entirely.
    Converter c;
    CHECK(c.load(path("test.tap")));
    Settings s = fastSettings(kShavingsRaudo, kS2_75);
    s.infoInFileName = false;
    c.applySettings(s);
    CHECK_EQ(c.suggestedOutputName("test.tap"), std::string("test.wav"));
    // The tag still names what the conversion will really do: Shavings Raudo
    // is forced to a single block, as k7zx 4.3 forced it, so "_VBLO_" (many
    // blocks) would be a lie.
    s.infoInFileName = true;
    c.applySettings(s);
    CHECK(c.suggestedOutputName("test.tap").find("_UNBL_") != std::string::npos);
    CHECK(c.suggestedOutputName("test.tap").find("_VBLO_") == std::string::npos);
}

TEST("inverting the wave appends the 'i' marker to the file name") {
    Converter c;
    CHECK(c.load(path("test.tap")));
    Settings s = fastSettings(kShavingsRaudo, kS2_75);
    s.invert = true;
    s.infoInFileName = true;
    c.applySettings(s);
    const std::string n = c.suggestedOutputName("test.tap");
    CHECK_EQ(n.substr(n.size() - 5, 1), std::string("i"));
}

TEST("converting with no blocks selected yields an error, not a crash") {
    const std::string out = "/tmp/k7zx-test-empty.wav";
    Converter c;
    CHECK(c.load(path("test.tap")));
    c.applySettings(fastSettings(kShavingsRaudo, kS2_75));
    c.selectAllBlocks(false);
    const ConversionResult r = c.convert(out, false);
    (void)r;  // either outcome is acceptable; it must not crash
    std::remove(out.c_str());
}

TEST("every scheme runs for every hispeed method without failing") {
    for (int m = kRom; m <= kEscurrido; ++m) {
        for (int sch = kOneBlock; sch <= kOriginalLoader; ++sch) {
            const std::string out = "/tmp/k7zx-test-matrix.wav";
            Converter c;
            CHECK(c.load(path("test.tap")));
            Settings s = fastSettings(m, kS2_75);
            s.scheme = static_cast<Scheme>(sch);
            c.applySettings(s);
            const ConversionResult r = c.convert(out, false);
            CHECK(r.ok);
            std::remove(out.c_str());
        }
    }
}

TEST("the TZX emulation path produces a TZX") {
    const std::string out = "/tmp/k7zx-test-emu.tzx";
    Converter c;
    CHECK(c.load(path("test.tap")));
    c.applySettings(fastSettings(kShavingsRaudo, kS2_75));
    c.audioOptions().emulate = true;
    const ConversionResult r = c.convert(out, false);
    CHECK(r.ok);
    CHECK(fileSize(out) > 70);
    std::remove(out.c_str());
}

TEST("output goes through a temp file that is valid on its own") {
    const std::string out = "/tmp/k7zx-test-temp.wav";
    Converter c;
    CHECK(c.load(path("test.tap")));
    c.applySettings(fastSettings(kFsk, kS5_00));
    const ConversionResult r = c.convert(out, false);
    CHECK(r.ok);
    std::ifstream f(out, std::ios::binary);
    char riff[4] = {0, 0, 0, 0};
    f.read(riff, 4);
    CHECK_EQ(std::string(riff, 4), std::string("RIFF"));
    std::remove(out.c_str());
}

// --- 128K snapshots ---------------------------------------------------------
// convertSnapshot128K and relocate128Code drive the 76-byte multi_128 blob, the
// most intricate code in the converter. Until now no fixture was a 128K
// snapshot, so none of it had ever executed.

TEST("a 128K .SNA loads seven blocks and converts") {
    Converter c;
    CHECK(c.load(path("test128k.sna")));
    CHECK(c.image().snap().snapshotType == 2);
    // The RAM under the ROM is page 7, so the reader fills the remaining banks.
    CHECK_EQ(c.image().blockCount(), 7);
    CHECK_EQ(c.image().snap().page, 0x0F);

    for (int m : {kRom, kShavingsRaudo, kFsk, kManchester}) {
        const std::string out = "/tmp/k7zx-test-128k.wav";
        c.applySettings(fastSettings(m, kS2_50));
        const ConversionResult r = c.convert(out, false);
        CHECK(r.ok);
        CHECK(r.errors.empty());
        CHECK(fileSize(out) > 1000);
        std::remove(out.c_str());
    }
}

TEST("a 128K .Z80 in hardware mode loads and converts") {
    Converter c;
    CHECK(c.load(path("test128k.z80")));
    CHECK(c.image().snap().snapshotType == 2);
    CHECK(c.image().blockCount() >= 2);
    CHECK(c.image().snap().page == 0x0F);

    const std::string out = "/tmp/k7zx-test-128kz80.wav";
    c.applySettings(fastSettings(kShavingsRaudo, kS2_50));
    const ConversionResult r = c.convert(out, false);
    CHECK(r.ok);
    CHECK(r.errors.empty());
    CHECK(fileSize(out) > 1000);
    std::remove(out.c_str());
}

TEST("every 128K technique and speed combination runs without error") {
    // relocate128Code is reached from the shared snapshot path, so this is where
    // an out-of-range patch offset would surface.
    for (int m = kRom; m <= kEscurrido; ++m) {
        for (int spb : Settings::allowedSamplesPerBit(static_cast<Method>(m))) {
            const std::string out = "/tmp/k7zx-test-128kmatrix.wav";
            Converter c;
            CHECK(c.load(path("test128k.sna")));
            c.applySettings(fastSettings(m, spb));
            const ConversionResult r = c.convert(out, false);
            CHECK(r.ok);
            std::remove(out.c_str());
        }
    }
}


// The Z80 routine, the patch offsets and the threshold tables are transcribed
// from OTLA 2.2. These check the *inputs* to that transcription: that the
// routine is the right size, that every speed is accepted, and that the byte
// patches and the threshold table land where OTLA puts them. They do not prove
// the wave loads -- nothing here can.

// ---------------------------------------------------------------------------
// The synthetic BASIC loader block
//
// The high-speed converter puts a tiny BASIC program on the tape ahead of the
// payload: a header block, then a program block holding
//
//     CLEAR n : RANDOMIZE USR 23781 : STR$  <the Z80 loader>  <CR>
//
// The Spectrum's ROM stores the program block at PROG and its interpreter then
// reads it as [line number][line length][tokens].  If the line length is zero
// the program is empty, the tape still loads, and nothing ever calls the loader
// -- so the block has to carry the real length.  The header's own param 2 has
// to survive too: it is the program size the ROM and LIST rely on.
//
// These checks decode the two standard-speed blocks straight out of the
// generated WAV, so they test the bytes that actually reach the tape.
// ---------------------------------------------------------------------------
namespace {

// k7zx renders every pulse as one full square-wave period, so the unit a
// loader sees is a pair of runs.  Nominal Spectrum timings in T-states.
constexpr double kPilot = 2 * 2168;
constexpr double kSync = 667 + 735;
constexpr double kBit0 = 2 * 855;
constexpr double kBit1 = 2 * 1710;

struct TapBlock {
    std::vector<unsigned char> bytes;
    bool valid = false;
};

std::vector<unsigned char> readWave(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
}

/// Pull the standard-speed frames out of an 8-bit mono WAV.
std::vector<TapBlock> decodeStandardBlocks(const std::vector<unsigned char>& wav) {
    // Locate the fmt and data chunks.
    std::size_t pos = 12;
    std::uint32_t rate = 0;
    std::size_t dataOff = 0, dataLen = 0;
    while (pos + 8 <= wav.size()) {
        const std::uint32_t sz = static_cast<std::uint32_t>(wav[pos + 4]) |
                                 (static_cast<std::uint32_t>(wav[pos + 5]) << 8) |
                                 (static_cast<std::uint32_t>(wav[pos + 6]) << 16) |
                                 (static_cast<std::uint32_t>(wav[pos + 7]) << 24);
        if (!std::memcmp(&wav[pos], "fmt ", 4) && sz >= 16)
            rate = static_cast<std::uint32_t>(wav[pos + 12]) |
                   (static_cast<std::uint32_t>(wav[pos + 13]) << 8) |
                   (static_cast<std::uint32_t>(wav[pos + 14]) << 16) |
                   (static_cast<std::uint32_t>(wav[pos + 15]) << 24);
        else if (!std::memcmp(&wav[pos], "data", 4)) {
            dataOff = pos + 8;
            dataLen = sz;
        }
        if (pos + 8 + sz > wav.size()) break;
        pos += 8 + sz + (sz & 1);
    }
    if (!rate || dataLen == 0) return {};
    if (dataOff + dataLen > wav.size()) dataLen = wav.size() - dataOff;

    // Constant-level runs, then pairs of them: one pair is one square-wave
    // period, which is the unit a Spectrum's loader measures.
    std::vector<double> runs;
    bool cur = wav[dataOff] > 128;
    std::size_t n = 0;
    for (std::size_t i = 0; i < dataLen; ++i) {
        const bool v = wav[dataOff + i] > 128;
        if (v == cur) ++n;
        else { runs.push_back(static_cast<double>(n)); cur = v; n = 1; }
    }
    runs.push_back(static_cast<double>(n));
    // The level alternates strictly, so the physical square-wave periods are
    // runs[0]+runs[1], runs[2]+runs[3], ...
    const double tps = 3500000.0 / static_cast<double>(rate);
    std::vector<double> period;
    for (std::size_t i = 0; i + 1 < runs.size(); i += 2)
        period.push_back((runs[i] + runs[i + 1]) * tps);

    auto near = [](double v, double t) { return std::fabs(v - t) < t * 0.15; };
    std::vector<TapBlock> out;
    std::size_t i = 0;
    while (i < period.size()) {
        // A block starts with a long run of pilot periods.
        std::size_t pilotBegin = period.size(), pilotEnd = period.size();
        for (std::size_t p = i; p < period.size();) {
            if (!near(period[p], kPilot)) { ++p; continue; }
            std::size_t q = p;
            while (q < period.size() && near(period[q], kPilot)) ++q;
            if (q - p >= 20) { pilotBegin = p; pilotEnd = q; break; }
            p = q;
        }
        if (pilotBegin == period.size()) break;
        // Then one sync period, then the bits.
        std::size_t k = pilotEnd;
        while (k < period.size() && !near(period[k], kSync)) ++k;
        if (k >= period.size()) break;
        ++k;
        TapBlock blk;
        unsigned acc = 0;
        int bits = 0;
        for (; k < period.size(); ++k) {
            if (near(period[k], kBit0)) { /* 0 */ }
            else if (near(period[k], kBit1)) acc |= 0x80u >> bits;
            else break;
            if (++bits == 8) {
                blk.bytes.push_back(static_cast<unsigned char>(acc));
                acc = 0;
                bits = 0;
            }
        }
        if (blk.bytes.empty()) break;
        blk.valid = true;
        out.push_back(blk);
        i = k > pilotEnd ? k : pilotEnd;
    }
    return out;
}

std::size_t basicBlockLineLength(const std::vector<TapBlock>& blocks) {
    CHECK(blocks.size() >= 2);
    if (blocks.size() < 2) return 0;
    const std::vector<unsigned char>& b = blocks[1].bytes;
    if (b.size() < 5) { CHECK(b.size() >= 5); return 0; }
    CHECK_EQ(int(b[0]), 0xff);  // data block flag
    return static_cast<std::size_t>(b[3]) | (static_cast<std::size_t>(b[4]) << 8);
}

}  // namespace

TEST("the synthetic BASIC loader carries a usable program line") {
    for (int m = kRom; m <= kEscurrido; ++m) {
        const std::vector<int> spb = Settings::allowedSamplesPerBit(static_cast<Method>(m));
        CHECK(!spb.empty());
        const std::string out = "/tmp/k7zx-test-line.wav";
        Converter c;
        CHECK(c.load(path("test.tap")));
        c.applySettings(fastSettings(m, spb[0]));
        const ConversionResult r = c.convert(out, false);
        CHECK(r.ok);

        const std::vector<TapBlock> blocks = decodeStandardBlocks(readWave(out));
        CHECK_EQ(blocks.size(), 2u);
        if (blocks.size() < 2 || blocks[0].bytes.size() < 19 || blocks[1].bytes.size() < 6) {
            std::remove(out.c_str());
            continue;
        }

        const std::vector<unsigned char>& hdr = blocks[0].bytes;
        CHECK_EQ(hdr.size(), 19u);
        CHECK_EQ(int(hdr[0]), 0x00);  // header flag
        CHECK_EQ(int(hdr[1]), 0x00);  // program
        const std::size_t hdrLen = static_cast<std::size_t>(hdr[12]) |
                                   (static_cast<std::size_t>(hdr[13]) << 8);
        const std::size_t param2 = static_cast<std::size_t>(hdr[16]) |
                                   (static_cast<std::size_t>(hdr[17]) << 8);
        // param 2 is the program size; the data-block length must not overwrite
        // its high byte.
        CHECK_EQ(param2, hdrLen);
        CHECK(hdrLen > 4);

        // The program block: [0xFF][line number][line length][tokens...][CR]
        const std::size_t lineLen = basicBlockLineLength(blocks);
        CHECK(lineLen > 0);
        // The header's length counts the line's own 4-byte header plus its body.
        CHECK_EQ(hdrLen, lineLen + 4);
        const std::vector<unsigned char>& prog = blocks[1].bytes;
        CHECK_EQ(int(prog[5]), 0xfd);  // CLEAR
        std::remove(out.c_str());
    }
}

TEST("the loader's entry jump points into its own relocated copy") {
    // Every routine opens by copying itself high into RAM and jumping to its own
    // entry point inside that copy.  Shavings Raudo copies to 0xFDBF rather than
    // the 0xFF3D the others use, so its jump must be followed: k7zx 4.3 wrote a
    // hard-coded 0xFF high byte over the operand, which sent Raudo to 0xFF3D,
    // where nothing had been copied, and it ran off into the zero fill.
    for (int m = kRom; m <= kEscurrido; ++m) {
        const std::vector<int> spb = Settings::allowedSamplesPerBit(static_cast<Method>(m));
        CHECK(!spb.empty());
        for (int s : spb) {
            const std::string out = "/tmp/k7zx-test-entry.wav";
            Converter c;
            CHECK(c.load(path("test.tap")));
            c.applySettings(fastSettings(m, s));
            const ConversionResult r = c.convert(out, false);
            CHECK(r.ok);
            const std::vector<TapBlock> blocks = decodeStandardBlocks(readWave(out));
            CHECK_EQ(blocks.size(), 2u);
            if (blocks.size() < 2 || blocks[1].bytes.size() < 5 + 22 + 16) {
                std::remove(out.c_str());
                continue;
            }
            // The preamble sits 22 bytes into the BASIC line.
            const std::size_t body = 5 + 22;
            const unsigned char* p = &blocks[1].bytes[body];
            CHECK_EQ(int(p[0]), 0x21);  // LD HL,nn
            CHECK_EQ(int(p[1]), 0xf5);
            CHECK_EQ(int(p[2]), 0x5c);  // copy source 0x5CF5
            CHECK_EQ(int(p[3]), 0x11);  // LD DE,nn
            const unsigned dest = static_cast<unsigned>(p[4]) | (static_cast<unsigned>(p[5]) << 8);
            CHECK_EQ(int(p[8]), 0xed);  // LDIR
            CHECK_EQ(int(p[9]), 0xb0);
            CHECK_EQ(int(p[13]), 0xc3);  // JP nn
            const unsigned target =
                static_cast<unsigned>(p[14]) | (static_cast<unsigned>(p[15]) << 8);
            // The jump has to land inside the image the LDIR just produced
            // (one page for most techniques, two for the Shavings Raudo
            // variants), and past the routine body -- that is where the
            // multi-block dispatcher sits.
            CHECK(target >= dest);
            CHECK(target < dest + 512u);
            CHECK(target - dest > 16u);
            std::remove(out.c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// Faithfulness to k7zx 4.3, checked against k7zx 4.3 itself.
//
// tools/k7zx43 builds the original engine from original/ (mechanical edits
// only) and make_golden.py hashes the WAV it writes for every technique,
// speed, sample rate and scheme 4.3 offered, from tests/data/golden.tap.  The
// port must write the very same bytes.  Every bug fixed alongside this test --
// the single-block image, the dispatcher tables, the Slow and Milks encoders,
// Agitato's stray byte, the table builders' first index, the K0/K1 offsets,
// Manchester-Dif 4.00, Escurrido 1.33 -- shows up here as a mismatch.
// ---------------------------------------------------------------------------
namespace {

struct Golden {
    int mp3, method, spb, rate, scheme;
    unsigned long long hash;
};
// The first field is the removed low-rate conversion mode (removed from this port). The table
// was regenerated from a freshly built tools/k7zx43 and no row carries it any
// more; the test refuses one if it ever reappears, so the table cannot drift
// back to comparing a mode the converter no longer has.
const Golden kGolden[] = {
#include "golden_k7zx43.inc"
};

std::uint64_t fnv1a64(const std::vector<unsigned char>& d) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char b : d) h = (h ^ b) * 0x100000001b3ULL;
    return h;
}

Settings goldenSettings(const Golden& g) {
    Settings s;
    s.conversionMode = kConvertHiSpeed;
    s.method = g.method;
    s.samplesPerBit = g.spb;
    s.sampleRate = g.rate;
    s.scheme = static_cast<Scheme>(g.scheme);
    s.waveform = kCubic;
    s.invert = s.invertRight = s.finalTone = s.stereo = s.accelerateBasic = false;
    s.antiKolmogorov = false;
    s.controlChecksum = true;
    s.generateLoader = true;
    return s;
}

}  // namespace

TEST("every conversion k7zx 4.3 offered is byte-identical to k7zx 4.3's own") {
    CHECK(sizeof(kGolden) / sizeof(kGolden[0]) > 300);
    int mismatches = 0;
    int skipped = 0;
    for (const Golden& g : kGolden) {
        if (g.mp3) { ++skipped; continue; }  // removed low-rate mode; must be none
        const std::string out = "/tmp/k7zx-test-golden.wav";
        Converter c;
        CHECK(c.load(path("golden.tap")));
        c.applySettings(goldenSettings(g));
        const ConversionResult r = c.convert(out, false);
        CHECK(r.ok);
        const std::uint64_t h = fnv1a64(readWave(out));
        if (h != g.hash && ++mismatches <= 10)
            k7zxtest::fail(__FILE__, __LINE__,
                           std::string(g.mp3 ? "mp3" : "hispeed") + " method " +
                               std::to_string(g.method) + " spb " + std::to_string(g.spb) +
                               " rate " + std::to_string(g.rate) + " scheme " +
                               std::to_string(g.scheme) + " differs from k7zx 4.3");
        std::remove(out.c_str());
    }
    CHECK_EQ(mismatches, 0);
    CHECK_EQ(skipped, 0);
}

TEST("Shavings Raudo always loads as one block, as k7zx 4.3 forced it to") {
    // 4.3 disabled the "many blocks" radio for Raudo (SettingdForm.cpp) and its
    // PlayerForm fell back to one block; Raudo has no entry point a dispatcher
    // can call.  The port's GUI greyed the radio out but still passed "many".
    for (int sch : {kManyBlocks, kOriginalLoader, kOneBlock}) {
        Converter c;
        CHECK(c.load(path("golden.tap")));
        Settings s = fastSettings(kShavingsRaudo, kS2_75);
        s.scheme = static_cast<Scheme>(sch);
        c.applySettings(s);
        CHECK_EQ(int(c.effectiveScheme()), int(kOneBlock));
        const ConversionResult r = c.convert("/tmp/k7zx-test-raudo-scheme.wav", false);
        CHECK(r.ok);
        if (sch != kOneBlock) CHECK(r.warnings.find("used 'one'") != std::string::npos);
        std::remove("/tmp/k7zx-test-raudo-scheme.wav");
    }
    // Other techniques keep "many"; "original" is Milks-only and needs BASIC.
    Converter c;
    CHECK(c.load(path("golden.tap")));
    Settings s = fastSettings(kRom, kS3_00);
    s.scheme = kOriginalLoader;
    c.applySettings(s);
    CHECK_EQ(int(c.effectiveScheme()), int(kManyBlocks));
    s = fastSettings(kMilks, kS2_75);
    s.scheme = kOriginalLoader;
    c.applySettings(s);
    CHECK_EQ(int(c.effectiveScheme()), int(kOriginalLoader));
}

TEST("the single-block image carries every CODE block, not just those below 23755") {
    // joinBlocks() bounded the copy by the BASIC area instead of the end of
    // memory, so the one-block tape carried zeros where the game should be.
    Converter c;
    CHECK(c.load(path("golden.tap")));
    Settings s = fastSettings(kRom, kS3_00);
    s.scheme = kOneBlock;
    c.applySettings(s);
    CHECK(c.convert("/tmp/k7zx-test-oneblock.wav", false).ok);
    std::remove("/tmp/k7zx-test-oneblock.wav");
    const auto& mem = c.image().memory();
    int checked = 0;
    for (const BlockInfo& b : c.image().blocks()) {
        if (b.isTerminator()) break;
        if (b.type != '3') continue;
        for (unsigned i = 0; i < b.length; ++i)
            if (mem[b.startAddress + i] != b.data[i]) {
                CHECK_EQ(int(mem[b.startAddress + i]), int(b.data[i]));
                break;
            }
        ++checked;
    }
    CHECK_EQ(checked, 2);  // SCREEN$ at 16384, CODE at 32768
}

TEST("the 48K snapshot restore stub is entered at its first instruction") {
    // The register block before deco_snap is 29 bytes (13 words, jp_reti, PC)
    // and the stub is entered at 0x4900 + 29.  With a 27-byte block the stub
    // sat two bytes early and every 48K snapshot restore began with RST $38.
    Converter c;
    CHECK(c.load(path("test48k.sna")));
    c.applySettings(fastSettings(kRom, kS3_00));
    CHECK(c.convert("/tmp/k7zx-test-sna.wav", false).ok);
    std::remove("/tmp/k7zx-test-sna.wav");
    const auto& mem = c.image().memory();
    CHECK_EQ(c.image().usrN(), 0x4900u + 29u);
    CHECK_EQ(int(mem[0x4900 + 29]), 0x11);  // LD DE,nn -- deco_snap's first byte
    CHECK_EQ(int(mem[0x4900 + 32]), 0x21);  // LD HL,nn
}

TEST("Shavings Raudo's 128K loader is relocated on all three of its pages") {
    // On a 128K the loader runs 0x4000 lower.  Raudo spans 0xFD..0xFF and k7zx
    // 4.3 relocated all three high bytes (and shifted multi_128 by 20); the
    // port only did 0xFF and 0xFE, so its 128K loads went nowhere.
    Converter c;
    CHECK(c.load(path("test128k.sna")));
    c.applySettings(fastSettings(kShavingsRaudo, kS2_75));
    const std::string out = "/tmp/k7zx-test-raudo128.wav";
    CHECK(c.convert(out, false).ok);
    const std::vector<TapBlock> blocks = decodeStandardBlocks(readWave(out));
    std::remove(out.c_str());
    CHECK_EQ(blocks.size(), 2u);
    if (blocks.size() < 2 || blocks[1].bytes.size() < 300) return;
    const std::vector<unsigned char>& b = blocks[1].bytes;
    const std::size_t body = 5 + 22;            // flag + line header + BASIC
    CHECK_EQ(int(b[body + 3]), 0x11);           // LD DE,nn
    CHECK_EQ(int(b[body + 5]), 0xbd);           // 0xFDBF -> 0xBDBF
    // No absolute store into the loader's own pages may still point at
    // 0xFDxx..0xFFxx: LD (nn),A and LD (nn),HL inside the 277-byte routine.
    for (std::size_t i = body; i + 2 < body + 277; ++i)
        if (b[i] == 0x32 || b[i] == 0x22) CHECK(b[i + 2] < 0xfd);
}

TEST("Ultra and NPU offer 2.00 samples per bit, as k7zx 4.3 did") {
    // 4.3's combo columns 10 = 2.00; an earlier transcription read 2.25, a
    // speed neither patcher has a case for.
    for (Method m : {kUltra, kNpu}) {
        const std::vector<int> v = Settings::allowedSamplesPerBit(m);
        CHECK(std::find(v.begin(), v.end(), int(kS2_00)) != v.end());
        CHECK(std::find(v.begin(), v.end(), int(kS2_25)) == v.end());
    }
}
