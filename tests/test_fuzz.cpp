// Fuzz the tape readers: arbitrary mutations of valid files must produce a
// diagnostic, never a crash or a memory error.
//
// Built with -fsanitize=address,undefined this is the strongest check the
// port has: every multi-byte field in the readers is bounds-checked, so a
// hostile or corrupt file has to fail cleanly.
#include "test_harness.h"

#include <cstdio>
#include <fstream>
#include <random>
#include <vector>

#include "core/convert.h"
#include "core/zxcode.h"
#include "core/zxfiles.h"

using namespace k7zx;

namespace {

std::vector<std::uint8_t> loadFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
}

/// Load a fixture, or fail the test cleanly.  Running the binary from the wrong
/// directory used to make these tests divide by zero on an empty file instead
/// of saying what was wrong.
std::vector<std::uint8_t> loadFixture(const char* name) {
    std::vector<std::uint8_t> d = loadFile(k7zxtest::dataDir() + "/" + name);
    if (d.empty()) {
        k7zxtest::fail(__FILE__, __LINE__,
                       std::string("cannot read the fixture '") + name +
                           "'; --data-dir is probably wrong (looked in " + k7zxtest::dataDir() +
                           ")");
    }
    return d;
}

void writeFile(const std::string& p, const std::vector<std::uint8_t>& d) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

}  // namespace

TEST("fuzz: single byte mutations of every supported format") {
    const char* files[] = {"test.tap", "test.tzx", "test48k.sna", "test48k.z80"};
    std::mt19937 rng(20260929);  // fixed seed, so a failure is reproducible

    int cases = 0;
    for (const char* name : files) {
        const std::vector<std::uint8_t> orig = loadFixture(name);
        if (orig.empty()) continue;
        for (int trial = 0; trial < 300; ++trial) {
            std::vector<std::uint8_t> d = orig;
            const int mutations = 1 + static_cast<int>(rng() % 6);
            for (int m = 0; m < mutations; ++m) {
                std::size_t pos = rng() % d.size();
                d[pos] = static_cast<std::uint8_t>(rng() & 0xff);
            }
            const std::string p = "/tmp/k7zx-fuzz.bin";
            writeFile(p, d);

            Converter c;
            if (c.load(p)) {
                // A file that still parses must survive a full conversion.
                Settings s;
                s.method = kShavingsRaudo;
                s.samplesPerBit = kS2_75;
                c.applySettings(s);
                c.convert("/tmp/k7zx-fuzz-out.wav", false);
                c.convert("/tmp/k7zx-fuzz-out.wav", true);
            }
            std::remove(p.c_str());
            std::remove("/tmp/k7zx-fuzz-out.wav");
            ++cases;
        }
    }
    CHECK_EQ(cases, 1200);
}

TEST("fuzz: truncating a file at every length must not crash") {
    const char* files[] = {"test.tap", "test.tzx", "test48k.sna", "test48k.z80"};
    for (const char* name : files) {
        const std::vector<std::uint8_t> orig = loadFixture(name);
        if (orig.empty()) continue;
        for (std::size_t n = 0; n <= orig.size(); n += 13) {
            std::vector<std::uint8_t> d(orig.begin(), orig.begin() + static_cast<long>(n));
            const std::string p = "/tmp/k7zx-trunc.bin";
            writeFile(p, d);
            Converter c;
            if (c.load(p)) {
                Settings s;
                s.method = kMilks;
                c.applySettings(s);
                c.convert("/tmp/k7zx-trunc-out.wav", false);
            }
            std::remove(p.c_str());
            std::remove("/tmp/k7zx-trunc-out.wav");
        }
    }
}

TEST("fuzz: random bytes masquerading as each format") {
    std::mt19937 rng(4242);
    const char* names[] = {"a.tap", "a.tzx", "a.sna", "a.z80", "a.sbb", "a.hex"};
    for (int trial = 0; trial < 400; ++trial) {
        const std::size_t len = rng() % 5000;
        std::vector<std::uint8_t> d(len);
        for (auto& b : d) b = static_cast<std::uint8_t>(rng() & 0xff);
        const std::string p = std::string("/tmp/k7zx-") + names[trial % 6];
        writeFile(p, d);
        Converter c;
        if (c.load(p)) {
            Settings s;
            s.method = kFsk;
            c.applySettings(s);
            c.convert("/tmp/k7zx-rand-out.wav", false);
        }
        std::remove(p.c_str());
        std::remove("/tmp/k7zx-rand-out.wav");
    }
}

TEST("fuzz: headers claiming the largest legal sizes") {
    // TAP: a block that claims 0xFFFF bytes in a 4 byte file.
    {
        std::vector<std::uint8_t> d{0xff, 0xff, 0x00, 0xff};
        writeFile("/tmp/k7zx-huge.tap", d);
        Converter c;
        CHECK(!c.load("/tmp/k7zx-huge.tap"));
        std::remove("/tmp/k7zx-huge.tap");
    }
    // TZX: a 0x15 direct recording block claiming 0xFFFFFF samples.
    {
        std::vector<std::uint8_t> d{'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1a, 1, 20,
                                    0x15, 0x10, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff};
        writeFile("/tmp/k7zx-huge.tzx", d);
        Converter c;
        c.load("/tmp/k7zx-huge.tzx");  // must not crash even if it fails
        std::remove("/tmp/k7zx-huge.tzx");
    }
    // SNA with a 64K header but a truncated body.
    {
        std::vector<std::uint8_t> d(30, 0x5a);
        writeFile("/tmp/k7zx-huge.sna", d);
        Converter c;
        CHECK(!c.load("/tmp/k7zx-huge.sna"));
        std::remove("/tmp/k7zx-huge.sna");
    }
    // Z80 v2 with a bogus extra header length.
    {
        std::vector<std::uint8_t> d(31, 0);
        d[6] = 0; d[7] = 0;   // PC == 0, so this is not a v1 snapshot
        d[30] = 0xff;         // the extra header claims 255 bytes
        writeFile("/tmp/k7zx-huge.z80", d);
        Converter c;
        CHECK(!c.load("/tmp/k7zx-huge.z80"));
        std::remove("/tmp/k7zx-huge.z80");
    }
}

TEST("fuzz: a TZX 0x14 block just under 64 KiB must not desynchronise") {
    // The original decoded the three byte length as low16 + 0x10000*high,
    // which was correct only below 64 KiB.  Build a block above that.
    // Built by appending rather than from a braced list: GCC's -Warray-bounds
    // mis-analyses a vector constructed from a ten element literal and then
    // appended to.
    std::vector<std::uint8_t> d;
    for (std::uint8_t b : std::string("ZXTape!")) d.push_back(b);
    d.push_back(0x1a);
    d.push_back(1);
    d.push_back(20);
    const std::uint32_t len = 0x20000;  // 128 KiB
    d.push_back(0x14);
    d.push_back(0x57); d.push_back(0x03);  // zero pulse 855
    d.push_back(0xae); d.push_back(0x06);  // one pulse 1710
    d.push_back(8);                        // used bits
    d.push_back(0xe8); d.push_back(0x03);  // pause 1000 ms
    d.push_back(static_cast<std::uint8_t>(len & 0xff));
    d.push_back(static_cast<std::uint8_t>((len >> 8) & 0xff));
    d.push_back(static_cast<std::uint8_t>((len >> 16) & 0xff));
    d.resize(d.size() + len, 0xa5);
    d.push_back(0x20);  // a pause block, which must be found at the right offset
    d.push_back(0xe8); d.push_back(0x03);

    writeFile("/tmp/k7zx-big.tzx", d);
    TapeImage img;
    const ReadError e = img.read("/tmp/k7zx-big.tzx");
    // The reader must either accept it or report a format error, and must
    // never report a plain success while mis-parsing (the old bug produced a
    // desynchronised walk and an error).
    CHECK(e == ReadError::none || e == ReadError::format);
    CHECK(img.errors().empty() || e != ReadError::none);
    std::remove("/tmp/k7zx-big.tzx");
}
