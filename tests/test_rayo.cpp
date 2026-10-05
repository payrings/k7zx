// Tests for the Rayo loader (added by this port): the LZ coder, the decode
// tables, the turbo stream and the BASIC line, plus the converter paths.
//
// What these cannot show is that the Z80 code loads the signal; that is what
// tools/zxload (an emulated 48K/128K running the real ROM and loader) and
// tools/channel/robustness.py are for -- see the README.
#include "test_harness.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "core/convert.h"
#include "core/lz.h"
#include "core/rayo.h"
#include "core/rayo_loader.inc"
#include "core/settings.h"
#include "core/zxfiles.h"

using namespace k7zx;

namespace {

std::string path(const char* name) { return k7zxtest::dataDir() + "/" + name; }

void writeBytes(const std::string& p, const std::vector<std::uint8_t>& d) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

// --- sample data -------------------------------------------------------------
std::vector<std::uint8_t> randomBytes(std::size_t n, unsigned seed) {
    std::mt19937 g(seed);
    std::vector<std::uint8_t> v(n);
    for (auto& b : v) b = static_cast<std::uint8_t>(g());
    return v;
}

// Something shaped like a Spectrum program: code-like bytes, a blank-ish
// screen, repeated sprite rows and a zero-filled tail.
std::vector<std::uint8_t> programLike(std::size_t n, unsigned seed) {
    std::mt19937 g(seed);
    std::vector<std::uint8_t> v;
    const std::uint8_t ops[] = {0x3e, 0x21, 0xcd, 0xc9, 0x7e, 0x23, 0x10, 0x18, 0x20, 0x28, 0xed, 0xb0, 0x00};
    while (v.size() < n) {
        switch (g() % 4) {
            case 0:  // code
                for (int k = 0; k < 40; ++k) v.push_back(ops[g() % sizeof ops]);
                break;
            case 1:  // a run
                v.insert(v.end(), 20 + g() % 300, static_cast<std::uint8_t>(g() % 3 ? 0 : 0xff));
                break;
            case 2: {  // repeat something earlier
                if (v.size() > 64) {
                    const std::size_t back = 1 + g() % std::min<std::size_t>(v.size(), 5000);
                    const std::size_t len = 3 + g() % 60;
                    const std::size_t from = v.size() - back;
                    for (std::size_t k = 0; k < len; ++k) v.push_back(v[from + k]);
                }
                break;
            }
            default:  // noise
                for (int k = 0; k < 16; ++k) v.push_back(static_cast<std::uint8_t>(g()));
        }
    }
    v.resize(n);
    return v;
}

// The format exactly as the Z80 expands it, but in ONE buffer: the stream sits
// at `src`, the output grows from `dst`.  If the in-place margin is too small,
// output overwrites stream bytes not yet read and the result is wrong.
bool expandInPlace(std::vector<std::uint8_t>& mem, std::size_t src, std::size_t dst) {
    std::uint8_t buf = 0, mask = 0;
    auto rbyte = [&]() { return mem.at(src++); };
    auto bit = [&]() {
        if (mask == 0) { buf = rbyte(); mask = 0x80; }
        const int b = (buf & mask) ? 1 : 0;
        mask >>= 1;
        return b;
    };
    auto gamma = [&]() {
        std::size_t v = 1;
        while (!bit()) v = (v << 1) | static_cast<std::size_t>(bit());
        return v;
    };
    std::size_t last = 0, len = gamma();
    for (std::size_t k = 0; k < len; ++k) mem.at(dst++) = rbyte();
    bool afterLiteral = true;
    for (int guard = 0; guard < 1000000; ++guard) {
        if (bit()) {
            const std::size_t h = gamma();
            if (h == 256) return true;
            last = (((h - 1) << 8) | rbyte()) + 1;
            len = gamma() + 1;
            for (std::size_t k = 0; k < len; ++k, ++dst) mem.at(dst) = mem.at(dst - last);
            afterLiteral = false;
        } else if (afterLiteral) {
            len = gamma();
            for (std::size_t k = 0; k < len; ++k, ++dst) mem.at(dst) = mem.at(dst - last);
            afterLiteral = false;
        } else {
            len = gamma();
            for (std::size_t k = 0; k < len; ++k) mem.at(dst++) = rbyte();
            afterLiteral = true;
        }
    }
    return false;
}

// The loader's timing model (rayo.cpp): R at the end of a cycle of T states.
// Mirrored here so the tests decode the stream the way the Z80 does.
int rFor(double samples, int preset, int rate, double cpu = 3500000.0) {
    const double T = samples * 3500000.0 / rate * (cpu / 3500000.0);
    return static_cast<int>(std::lround(preset - 19.0 + 3.0 / 16.0 * T)) & 0x7f;
}

// Split a built program's turbo section into bytes of cycle-slot codes
// (0 = shortest cycle .. 3 = longest), checking the framing on the way.
bool decodeCycles(const rayo::Program& p, int pmin, std::vector<std::uint8_t>& out) {
    out.clear();
    std::size_t i = 0;
    while (i < p.cycles.size() && p.cycles[i].first == 8 && p.cycles[i].second == 8) ++i;  // pilot
    if (i < 100 || i >= p.cycles.size()) return false;
    if (p.cycles[i].first != 2 || p.cycles[i].second != 8) return false;                    // sync
    unsigned acc = 1;
    for (++i; i < p.cycles.size(); ++i) {
        const int len = p.cycles[i].first + p.cycles[i].second;
        if (len == pmin + 5) return acc == 1;  // the terminator, on a byte boundary
        if (len < pmin || len > pmin + 3) return false;
        acc = (acc << 2) | static_cast<unsigned>(len - pmin);
        if (acc & 0x100) {
            out.push_back(static_cast<std::uint8_t>(acc & 0xff));
            acc = 1;
        }
    }
    return false;
}

Settings rayoSettings(int spb, bool compress) {
    Settings s;
    s.conversionMode = kConvertHiSpeed;
    s.method = kRayo;
    s.samplesPerBit = spb;
    s.sampleRate = 48000;
    s.scheme = kOneBlock;
    s.waveform = kSquare;
    s.generateLoader = true;
    s.compress = compress;
    return s;
}

std::size_t fileSize(const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    return f ? static_cast<std::size_t>(f.tellg()) : 0;
}

}  // namespace

// --- LZ ------------------------------------------------------------------------

TEST("LZ: compress / decompress round-trips assorted data") {
    std::vector<std::vector<std::uint8_t>> cases;
    cases.push_back({0x42});
    cases.push_back({1, 2});
    cases.push_back(std::vector<std::uint8_t>(1000, 0));
    cases.push_back(std::vector<std::uint8_t>(70000, 0xe5));  // longer than one match
    cases.push_back(randomBytes(3000, 1));
    cases.push_back(programLike(40000, 2));
    cases.push_back(programLike(6912, 3));
    std::vector<std::uint8_t> period;
    for (int i = 0; i < 20000; ++i) period.push_back(static_cast<std::uint8_t>("ABCDEFG"[i % 7]));
    cases.push_back(period);
    for (const auto& d : cases) {
        const auto c = lz::compress(d.data(), d.size());
        std::vector<std::uint8_t> out;
        CHECK(lz::decompress(c.data(), c.size(), out, nullptr, 0x20000));
        CHECK(out == d);
    }
}

TEST("LZ: redundant data shrinks, random data does not grow much") {
    const auto zeros = std::vector<std::uint8_t>(40000, 0);
    CHECK(lz::compress(zeros.data(), zeros.size()).size() < 40);
    const auto prog = programLike(40000, 4);
    CHECK(lz::compress(prog.data(), prog.size()).size() < prog.size() * 3 / 4);
    const auto rnd = randomBytes(10000, 5);
    CHECK(lz::compress(rnd.data(), rnd.size()).size() < rnd.size() + rnd.size() / 50);
}

TEST("LZ: the in-place margin is enough to expand over the stream itself") {
    for (unsigned seed = 10; seed < 16; ++seed) {
        const auto d = seed % 2 ? programLike(30000, seed) : randomBytes(2000 + seed * 100, seed);
        const auto c = lz::compress(d.data(), d.size());
        int margin = lz::inPlaceMargin(d.data(), d.size(), c);
        CHECK(margin >= 0);
        // (random data compresses to more than it was; Rayo then sends it
        // raw, but the stream must still start at or after the data here)
        margin = std::max(margin, static_cast<int>(c.size()) - static_cast<int>(d.size()));
        // Lay memory out as the loader does: data at 0, stream ending margin
        // bytes past the data's end.
        std::vector<std::uint8_t> mem(d.size() + static_cast<std::size_t>(margin) + 4, 0xaa);
        const std::size_t src = d.size() + static_cast<std::size_t>(margin) - c.size();
        std::copy(c.begin(), c.end(), mem.begin() + static_cast<long>(src));
        CHECK(expandInPlace(mem, src, 0));
        CHECK(std::equal(d.begin(), d.end(), mem.begin()));
    }
}

TEST("LZ: the decompressor rejects damaged streams instead of overrunning") {
    const auto d = programLike(5000, 20);
    auto c = lz::compress(d.data(), d.size());
    std::vector<std::uint8_t> out;
    // truncated
    CHECK(!lz::decompress(c.data(), c.size() / 2, out));
    // an offset that reaches before the start: a new-offset match first
    const std::uint8_t early[] = {0x80 | 0x40, 0x07, 0xff, 0xff};  // literal 1, then a far match
    CHECK(!lz::decompress(early, sizeof early, out, nullptr, 1000));
    // garbage never runs away
    for (unsigned s = 0; s < 50; ++s) {
        const auto g = randomBytes(64, 100 + s);
        lz::decompress(g.data(), g.size(), out, nullptr, 100000);
        CHECK(out.size() <= 100001 + 0x20001);
    }
    // a wrong margin check: a stream that does not reproduce the data
    auto other = d;
    other[100] ^= 1;
    CHECK_EQ(lz::inPlaceMargin(other.data(), other.size(), c), -1);
}

// --- tables -------------------------------------------------------------------

TEST("Rayo: every data cycle decodes to its slot at 48K and 128K timing") {
    for (int rate : {44100, 48000}) {
        for (int pmin : {3, 4}) {
            const std::array<int, 4> map{2, 0, 3, 1};
            const rayo::Tables t = rayo::tables(rate, pmin, map);
            for (double cpu : {3500000.0, 3546900.0}) {
                for (int s = 0; s < 4; ++s) {
                    const int r = rFor(pmin + s, t.preset, rate, cpu);
                    CHECK_EQ(int(t.table[static_cast<std::size_t>(r)]), map[static_cast<std::size_t>(s)]);
                }
                // the terminator, pmin + 5 samples, ends the stream
                CHECK_EQ(int(t.table[static_cast<std::size_t>(rFor(pmin + 5, t.preset, rate, cpu))]), 0x80);
            }
            // R 4..32 is the sync sentinel; 0..3 never occur
            for (int r = 4; r <= 32; ++r) CHECK_EQ(int(t.table[static_cast<std::size_t>(r)]), 1);
        }
    }
}

TEST("Rayo: data cycles keep a margin either side of each window") {
    // Half a sample of jitter in either direction must not change the symbol.
    for (int rate : {44100, 48000}) {
        for (int pmin : {3, 4}) {
            const rayo::Tables t = rayo::tables(rate, pmin, {0, 1, 2, 3});
            for (int s = 0; s < 4; ++s) {
                for (double d : {-0.4, 0.4}) {
                    const int r = rFor(pmin + s + d, t.preset, rate);
                    CHECK_EQ(int(t.table[static_cast<std::size_t>(r)]), s);
                }
            }
        }
    }
}

// --- the program ------------------------------------------------------------------

TEST("Rayo: raw build -- stream, checksum and BASIC line") {
    const auto d = programLike(20000, 30);
    rayo::Request rq;
    rq.sampleRate = 48000;
    rq.pmin = 3;
    rq.data = d.data();
    rq.dest = 0x6000;
    rq.length = static_cast<unsigned>(d.size());
    rq.compress = false;
    rq.usr = 0x6000;
    rayo::Program p;
    std::string err;
    CHECK(rayo::build(rq, p, err));
    CHECK(err.empty());
    CHECK(!p.compressed);
    CHECK_EQ(p.loadAddress, 0x6000u);
    CHECK_EQ(p.sentBytes, d.size() + 1);

    // the stream: pilot, sync, 4 cycles a byte, terminator, tail
    std::vector<std::uint8_t> slots;
    CHECK(decodeCycles(p, 3, slots));
    CHECK_EQ(slots.size(), d.size() + 1);
    // slots are cycle-length codes; map them back through the per-tape map,
    // which puts the commonest 2-bit value on the shortest cycle
    std::array<long, 4> count{};
    for (std::uint8_t b : d)
        for (int sh = 6; sh >= 0; sh -= 2) ++count[(b >> sh) & 3];
    std::uint8_t sum = 0;
    for (std::uint8_t b : d) sum ^= b;
    for (int sh = 6; sh >= 0; sh -= 2) ++count[(sum >> sh) & 3];
    std::array<int, 4> slotValue{0, 1, 2, 3};
    std::stable_sort(slotValue.begin(), slotValue.end(), [&](int a, int b) { return count[a] > count[b]; });
    std::vector<std::uint8_t> bytes;
    for (std::uint8_t s : slots) {
        std::uint8_t v = 0;
        for (int sh = 6; sh >= 0; sh -= 2) v |= static_cast<std::uint8_t>(slotValue[(s >> sh) & 3] << sh);
        bytes.push_back(v);
    }
    CHECK(std::equal(d.begin(), d.end(), bytes.begin()));
    std::uint8_t all = 0;
    for (std::uint8_t b : bytes) all ^= b;
    CHECK_EQ(int(all), 0);  // the stored checksum makes the XOR of everything 0

    // the BASIC line: number 0, length, CLEAR ... RANDOMIZE USR 23781, boot at 23781
    CHECK(p.line.size() > 4 + 22 + sizeof rayo::kBoot);
    CHECK_EQ(int(p.line[0]), 0);
    CHECK_EQ(int(p.line[1]), 0);
    CHECK_EQ(std::size_t(p.line[2] | (p.line[3] << 8)), p.line.size() - 4);
    CHECK_EQ(int(p.line[4]), 0xfd);  // CLEAR
    CHECK_EQ(std::string(p.line.begin() + 12, p.line.begin() + 24), std::string("\":\xf9\xc0\xb0\"23781\""));
    CHECK(std::equal(rayo::kBoot, rayo::kBoot + sizeof rayo::kBoot, p.line.begin() + 26));
    CHECK_EQ(int(p.line.back()), 0x0d);
    CHECK(23755u + p.line.size() < 0x6000u);  // the line clears the data it loads
}

TEST("Rayo: compressed build expands to the data, in place, below the loader") {
    const auto d = programLike(30000, 40);
    rayo::Request rq;
    rq.data = d.data();
    rq.dest = 0x8000;
    rq.length = static_cast<unsigned>(d.size());
    rq.compress = true;
    rayo::Program p;
    std::string err;
    CHECK(rayo::build(rq, p, err));
    CHECK(p.compressed);
    CHECK(p.sentBytes < d.size());
    CHECK(p.loadAddress + p.sentBytes <= rayo::k_dzxstart);
    CHECK(p.loadAddress > rq.dest);

    // Rebuild what landed in memory and expand it in place.
    rayo::Request raw = rq;
    raw.compress = false;
    const auto c = lz::compress(d.data(), d.size());
    CHECK_EQ(p.sentBytes, c.size() + 1);
    std::vector<std::uint8_t> mem(0x10000, 0x55);
    std::copy(c.begin(), c.end(), mem.begin() + p.loadAddress);
    CHECK(expandInPlace(mem, p.loadAddress, rq.dest));
    CHECK(std::equal(d.begin(), d.end(), mem.begin() + rq.dest));

    // the compressed stream needs fewer cycles than the raw one
    rayo::Program q;
    CHECK(rayo::build(raw, q, err));
    CHECK(p.cycles.size() < q.cycles.size());
}

TEST("Rayo: incompressible data or no headroom falls back to raw with a warning") {
    const auto rnd = randomBytes(8000, 50);
    rayo::Request rq;
    rq.data = rnd.data();
    rq.dest = 0x8000;
    rq.length = static_cast<unsigned>(rnd.size());
    rayo::Program p;
    std::string err;
    CHECK(rayo::build(rq, p, err));
    CHECK(!p.compressed);
    CHECK(!p.warnings.empty());

    // Data right up to the loader (a 48K snapshot): the stream's tail may
    // run on over the pilot-search code, which is finished with by then, but
    // never into the decompressor; when it would, the data goes raw.
    const auto prog = programLike(0x4000, 51);
    rq.data = prog.data();
    rq.length = static_cast<unsigned>(prog.size());
    rq.dest = rayo::loaderBase() - rq.length;
    CHECK(rayo::build(rq, p, err));
    if (p.compressed)
        CHECK(p.loadAddress + p.sentBytes <= rayo::k_dzxstart);
    else
        CHECK(p.warnings.find("room") != std::string::npos);
}

TEST("Rayo: requests it cannot meet are refused") {
    std::vector<std::uint8_t> d(100, 1);
    rayo::Request rq;
    rq.data = d.data();
    rq.length = 100;
    rayo::Program p;
    std::string err;
    rq.dest = rayo::loaderBase() - 50;  // overlaps the loader
    CHECK(!rayo::build(rq, p, err));
    CHECK(!err.empty());
    rq.dest = 0x3000;  // ROM
    CHECK(!rayo::build(rq, p, err));
    rq.dest = 0x8000;
    rq.pmin = 2;
    CHECK(!rayo::build(rq, p, err));
    rq.pmin = 3;
    rq.sampleRate = 22050;
    CHECK(!rayo::build(rq, p, err));
}

// --- through the converter ---------------------------------------------------------

TEST("Rayo: tapes and 48K snapshots convert, with and without compression") {
    for (const char* f : {"test.tap", "test48k.sna", "test48k.z80"}) {
        for (int spb : Settings::allowedSamplesPerBit(kRayo)) {
            for (bool comp : {false, true}) {
                Converter c;
                CHECK(c.load(path(f)));
                c.applySettings(rayoSettings(spb, comp));
                const std::string out = "/tmp/k7zx-test-rayo.wav";
                const ConversionResult r = c.convert(out, false);
                CHECK(r.ok);
                CHECK(r.errors.empty());
                CHECK(fileSize(out) > 1000);
                std::remove(out.c_str());
            }
        }
    }
}

TEST("Rayo: a 128K snapshot is refused with a pointer to Shavings Raudo") {
    Converter c;
    CHECK(c.load(path("test128k.sna")));
    c.applySettings(rayoSettings(kS2_25, true));
    const std::string out = "/tmp/k7zx-test-rayo128.wav";
    const ConversionResult r = c.convert(out, false);
    CHECK(!r.ok);
    CHECK(r.errors.find("Raudo") != std::string::npos);
    std::remove(out.c_str());
}

// --- the .Z80 v2/v3 reader (fixed alongside Rayo) ----------------------------------

TEST("Z80: a version 2 128K snapshot gives the PC and paging from the extra header") {
    TapeImage img;
    CHECK(img.read(path("test128k.z80")) == ReadError::none);
    CHECK_EQ(int(img.snap().snapshotType), 2);
    CHECK_EQ(img.snap().PC, 0x8000);
    CHECK_EQ(int(img.snap().page), 0x0F);
}

TEST("Z80: a version 3 48K snapshot, with a page stored uncompressed, reads correctly") {
    std::vector<std::uint8_t> f(30, 0);
    f[0] = 0x44;                      // A
    f[8] = 0x00; f[9] = 0xff;         // SP
    f[12] = 0x02;                     // border 1
    f[27] = f[28] = 1;
    f[29] = 1;                        // IM 1
    // extra header: length 54 (version 3), PC $9ABC, hardware 0 (48K)
    f.push_back(54); f.push_back(0);
    std::vector<std::uint8_t> extra(54, 0);
    extra[0] = 0xbc; extra[1] = 0x9a;
    extra[2] = 0;
    f.insert(f.end(), extra.begin(), extra.end());
    auto page = [&](int num, std::uint8_t seed, bool rawStore) {
        std::vector<std::uint8_t> raw(0x4000);
        for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<std::uint8_t>(i * seed + seed);
        // ED ED bytes in raw data would be runs to the RLE decoder: keep one in
        raw[10] = raw[11] = 0xed;
        if (rawStore) {
            f.push_back(0xff); f.push_back(0xff);
            f.push_back(static_cast<std::uint8_t>(num));
            f.insert(f.end(), raw.begin(), raw.end());
        } else {
            // RLE-encode: ED ED 02 ED for the pair, everything else literal
            std::vector<std::uint8_t> c;
            for (std::size_t i = 0; i < raw.size(); ++i) {
                if (i == 10) { c.insert(c.end(), {0xed, 0xed, 0x02, 0xed}); ++i; continue; }
                c.push_back(raw[i]);
            }
            f.push_back(static_cast<std::uint8_t>(c.size() & 0xff));
            f.push_back(static_cast<std::uint8_t>(c.size() >> 8));
            f.push_back(static_cast<std::uint8_t>(num));
            f.insert(f.end(), c.begin(), c.end());
        }
        return raw;
    };
    const auto p8 = page(8, 3, false);   // $4000
    const auto p4 = page(4, 5, true);    // $8000, stored raw (length $FFFF)
    const auto p5 = page(5, 7, false);   // $C000
    const std::string p = "/tmp/k7zx-test-v3.z80";
    writeBytes(p, f);
    TapeImage img;
    CHECK(img.read(p) == ReadError::none);
    CHECK_EQ(int(img.snap().snapshotType), 1);
    CHECK_EQ(img.snap().PC, 0x9abc);
    CHECK(std::equal(p8.begin(), p8.end(), img.pages()[5].begin()));
    CHECK(std::equal(p4.begin(), p4.end(), img.pages()[2].begin()));
    CHECK(std::equal(p5.begin(), p5.end(), img.pages()[0].begin()));
    std::remove(p.c_str());
}
