// Tests for the tape image readers.
#include "test_harness.h"

#include <cstdio>
#include <fstream>

#include "core/convert.h"
#include "core/zxfiles.h"

using namespace k7zx;

namespace {

std::string path(const char* name) { return k7zxtest::dataDir() + "/" + name; }

std::vector<std::uint8_t> makeTap() {
    // One header block and one code block, hand-built so the expectations below
    // are explicit.
    std::vector<std::uint8_t> out;
    auto word = [&out](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v & 0xff));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    auto block = [&](std::uint8_t flag, const std::vector<std::uint8_t>& body) {
        word(static_cast<std::uint16_t>(body.size() + 1));
        out.push_back(flag);
        out.insert(out.end(), body.begin(), body.end());
    };

    // The block() helper writes the flag byte itself, so the body starts at
    // the type byte.
    std::vector<std::uint8_t> header;
    header.push_back(0x03);                 // type: code
    for (int i = 0; i < 10; ++i) header.push_back(i < 4 ? 't' : 'e');
    auto push16 = [&](std::vector<std::uint8_t>& v, std::uint16_t x) {
        v.push_back(static_cast<std::uint8_t>(x & 0xff));
        v.push_back(static_cast<std::uint8_t>(x >> 8));
    };
    push16(header, 16);   // length
    push16(header, 0x8000);
    push16(header, 0);
    block(0x00, header);

    std::vector<std::uint8_t> payload(16);
    for (int i = 0; i < 16; ++i) payload[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i * 7);
    block(0xFF, payload);
    return out;
}

void writeFile(const std::string& p, const std::vector<std::uint8_t>& d) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

}  // namespace

TEST("TAP: header and data blocks are paired up") {
    const std::string p = "/tmp/k7zx-test-tap.tap";
    writeFile(p, makeTap());
    TapeImage img;
    CHECK(img.read(p) == ReadError::none);
    CHECK_EQ(img.blockCount(), 1);
    const BlockInfo& z = img.blocks()[0];
    CHECK_EQ(int(z.type), '3');
    CHECK_EQ(z.startAddress, 0x8000);
    CHECK_EQ(z.length, 16);
    CHECK(z.data != nullptr);
    for (int i = 0; i < 16; ++i) CHECK_EQ(int(z.data[i]), i * 7);
    std::remove(p.c_str());
}

TEST("TAP: a data block with no header is flagged as type 5") {
    std::vector<std::uint8_t> d;
    d.push_back(5);
    d.push_back(0);
    d.push_back(0xFF);  // data flag
    for (int i = 0; i < 4; ++i) d.push_back(static_cast<std::uint8_t>(i));
    const std::string p = "/tmp/k7zx-test-orphan.tap";
    writeFile(p, d);
    TapeImage img;
    CHECK(img.read(p) == ReadError::none);
    CHECK_EQ(img.blockCount(), 1);
    CHECK_EQ(int(img.blocks()[0].type), '5');
    CHECK(img.warnings().find("without header") != std::string::npos);
    std::remove(p.c_str());
}

TEST("TAP: an unknown block flag is a format error, not a crash") {
    std::vector<std::uint8_t> d;
    d.push_back(2);
    d.push_back(0);
    d.push_back(0x42);  // neither 0x00 nor 0xFF
    d.push_back(0x00);
    const std::string p = "/tmp/k7zx-test-bad.tap";
    writeFile(p, d);
    TapeImage img;
    CHECK(img.read(p) == ReadError::format);
    CHECK(!img.errors().empty());
    std::remove(p.c_str());
}

TEST("TAP: a block length past the end of the file is rejected") {
    std::vector<std::uint8_t> d;
    d.push_back(0xFF);
    d.push_back(0xFF);  // claims 65535 bytes
    d.push_back(0x00);
    const std::string p = "/tmp/k7zx-test-overrun.tap";
    writeFile(p, d);
    TapeImage img;
    CHECK(img.read(p) == ReadError::format);
    std::remove(p.c_str());
}

TEST("BASIC: CLEAR and USR are recovered from a tokenised program") {
    // 20 CLEAR 30000: LOAD ""CODE   /  30 RANDOMIZE USR 32768:
    std::vector<std::uint8_t> prog;
    auto line = [&](std::uint16_t number, const std::vector<std::uint8_t>& body) {
        const std::uint16_t len = static_cast<std::uint16_t>(body.size() + 1);
        prog.push_back(static_cast<std::uint8_t>(number & 0xff));
        prog.push_back(static_cast<std::uint8_t>(number >> 8));
        prog.push_back(static_cast<std::uint8_t>(len & 0xff));
        prog.push_back(static_cast<std::uint8_t>(len >> 8));
        prog.insert(prog.end(), body.begin(), body.end());
        prog.push_back(0x0d);  // CR
    };
    auto append = [](std::vector<std::uint8_t>& v, const char* s) {
        for (const char* p = s; *p; ++p) v.push_back(static_cast<std::uint8_t>(*p));
    };
    std::vector<std::uint8_t> l20{0xFD, 0xB0};
    append(l20, "30000");
    l20.push_back(0xB0);
    l20.push_back(':');
    l20.push_back(0xEF);
    l20.push_back(0xB0);
    l20.push_back(0xB0);
    l20.push_back(0xAF);
    line(20, l20);

    std::vector<std::uint8_t> l30{0xF9, 0xC0, 0xB0};
    append(l30, "32768");
    l30.push_back(0xB0);
    l30.push_back(':');  // statement separator
    line(30, l30);

    std::vector<std::uint8_t> d;
    auto w16 = [&d](std::uint16_t v) {
        d.push_back(static_cast<std::uint8_t>(v & 0xff));
        d.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    auto p16 = [](std::vector<std::uint8_t>& v, std::uint16_t x) {
        v.push_back(static_cast<std::uint8_t>(x & 0xff));
        v.push_back(static_cast<std::uint8_t>(x >> 8));
    };
    const std::uint16_t blockLen = static_cast<std::uint16_t>(1 + prog.size());
    std::vector<std::uint8_t> header;
    header.push_back(0x00);              // type: BASIC
    for (int i = 0; i < 10; ++i) header.push_back(0);
    p16(header, prog.size());
    p16(header, 30);                     // autostart line
    p16(header, 0);                      // vars offset
    header.push_back(0);                  // checksum, ignored on read
    w16(19);                              // header block length
    d.push_back(0x00);                    // header flag
    d.insert(d.end(), header.begin(), header.end());
    w16(blockLen);                       // data block length
    d.push_back(0xFF);                    // data flag
    d.insert(d.end(), prog.begin(), prog.end());

    const std::string p = "/tmp/k7zx-test-basic.tap";
    writeFile(p, d);
    TapeImage img;
    CHECK(img.read(p) == ReadError::none);
    CHECK_EQ(img.clearN(), 30000u);
    CHECK_EQ(img.usrN(), 32768u);
    std::remove(p.c_str());
}

TEST("TZX: a 0x14 pure-data block is skipped, not unpacked") {
    // k7zx only decodes 0x10 Standard Speed Data blocks; 0x14 carries raw
    // bytes with no timing, so it is stepped over and reported rather than
    // unpacked into tape blocks.
    TapeImage img;
    CHECK(img.read(path("test.tzx")) == ReadError::none);
    CHECK(img.warnings().find("non standard block") != std::string::npos);
    CHECK_EQ(img.blockCount(), 0);
}

TEST("TZX: a truncated file is a format error, not a crash") {
    const std::string src = path("test.tzx");
    std::ifstream in(src, std::ios::binary);
    std::vector<std::uint8_t> d((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    for (std::size_t cut = 11; cut < d.size(); cut += 37) {
        std::vector<std::uint8_t> t(d.begin(), d.begin() + static_cast<long>(cut));
        const std::string p = "/tmp/k7zx-test-trunc.tzx";
        writeFile(p, t);
        TapeImage img;
        const ReadError e = img.read(p);
        // Either it parses what is there or it reports a format error; it must
        // never read out of bounds or hang.
        CHECK(e == ReadError::none || e == ReadError::format);
    }
    std::remove("/tmp/k7zx-test-trunc.tzx");
}

TEST("TZX: the whole file can be truncated at any offset without crashing") {
    const char* files[] = {"test.tap", "test.tzx", "test48k.sna", "test48k.z80"};
    for (const char* f : files) {
        std::ifstream in(path(f), std::ios::binary);
        std::vector<std::uint8_t> d((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
        for (std::size_t cut = 1; cut < d.size(); cut += 997) {
            std::vector<std::uint8_t> t(d.begin(), d.begin() + static_cast<long>(cut));
            const std::string p = "/tmp/k7zx-test-cut.bin";
            writeFile(p, t);
            TapeImage img;
            img.read(p);  // must not crash
        }
    }
    std::remove("/tmp/k7zx-test-cut.bin");
}

TEST("SNA: a 48K snapshot is recognised and its registers recovered") {
    TapeImage img;
    CHECK(img.read(path("test48k.sna")) == ReadError::none);
    CHECK_EQ(int(img.snap().snapshotType), 1);
    CHECK_EQ(img.snap().PC, 0x4545);   // the original hard-codes this for 48K
    CHECK_EQ(img.snap().HL, 0x1234);
    CHECK_EQ(img.snap().DE, 0x5678);
    CHECK_EQ(img.snap().BC, 0x00FF);
    CHECK_EQ(img.snap().AF, 0x00FE);
    CHECK_EQ(img.snap().SP, 0xFF57);
    CHECK_EQ(img.snap().HLx, 0xFF56);
    CHECK_EQ(img.snap().DEx, 0xFF57);
    CHECK_EQ(img.snap().BCx, 0xFF58);
    CHECK_EQ(img.snap().AFx, 0xFF59);
    CHECK_EQ(img.snap().IY, 0xFF5A);
    CHECK_EQ(img.snap().IX, 0xFF5B);
    CHECK_EQ(img.snap().IR, 0x3F11);   // I in the high byte, R in the low
    CHECK_EQ(int(static_cast<std::uint8_t>(img.snap().jp_reti)), 0xed);  // RETI
    CHECK_EQ(img.blockCount(), 1);
    CHECK_EQ(img.blocks()[0].startAddress, 0x4000);
}

TEST("Z80: a version 1 48K snapshot is recognised") {
    TapeImage img;
    CHECK(img.read(path("test48k.z80")) == ReadError::none);
    CHECK_EQ(int(img.snap().snapshotType), 1);
    CHECK_EQ(img.snap().PC, 0x1234);
    CHECK_EQ(img.snap().AF, 0x00FE);
    CHECK_EQ(img.snap().BC, 0x00FF);
    CHECK_EQ(img.snap().HL, 0x1234);
    CHECK_EQ(img.snap().DE, 0xFF57);
    CHECK_EQ(img.snap().SP, 0xFFFF);
    CHECK_EQ(img.snap().HLx, 0xFF5A);
    CHECK_EQ(img.snap().DEx, 0xFF59);
    CHECK_EQ(img.snap().BCx, 0xFF58);
    CHECK_EQ(img.snap().AFx, 0xFF5B);
}

TEST("SNA: the decompressed page 5 holds the file contents") {
    TapeImage img;
    CHECK(img.read(path("test48k.sna")) == ReadError::none);
    // The reader decompresses into the page array; the working memory is only
    // assembled later, when the converter runs preparamemoriasnap().
    for (int i = 0; i < 256; ++i) CHECK_EQ(int(img.pages()[5][static_cast<std::size_t>(i)]), (i * 7) & 0xFF);
    for (int i = 0; i < 256; ++i) CHECK_EQ(int(img.pages()[2][static_cast<std::size_t>(i)]), (i * 13 + 5) & 0xFF);
}

TEST("SNA: preparing the snapshot memory copies the pages into place") {
    TapeImage img;
    CHECK(img.read(path("test48k.sna")) == ReadError::none);
    img.prepareSnapshotMemoryForConverter(false);
    // 48K snapshot, full (not shortened) variant: 0x4000..0xff00 is covered.
    for (int i = 0; i < 256; ++i) CHECK_EQ(int(img.memory()[0x4000 + i]), (i * 7) & 0xFF);
    for (int i = 0; i < 256; ++i) CHECK_EQ(int(img.memory()[0x8000 + i]), (i * 13 + 5) & 0xFF);
}

TEST("SNA: a size that matches no known layout is rejected") {
    std::vector<std::uint8_t> d(1000, 0x5a);
    const std::string p = "/tmp/k7zx-test-bad.sna";
    writeFile(p, d);
    TapeImage img;
    CHECK(img.read(p) == ReadError::format);
    std::remove(p.c_str());
}

TEST("an unknown extension is rejected") {
    const std::string p = "/tmp/k7zx-test-thing.bin";
    writeFile(p, std::vector<std::uint8_t>(64, 0));
    TapeImage img;
    CHECK(img.read(p) == ReadError::format);
    CHECK(img.errors().find("extension") != std::string::npos);
    std::remove(p.c_str());
}

TEST("a missing file reports an open error") {
    TapeImage img;
    CHECK(img.read("/tmp/k7zx-does-not-exist.tap") == ReadError::open);
}

TEST("z80 RLE decompression expands ED ED runs") {
    std::uint8_t out[32];
    std::memset(out, 0, sizeof out);
    const std::uint8_t in[] = {0x11, 0xED, 0xED, 0x03, 0xAA, 0x22};
    z80Decompress(out, sizeof out, in, sizeof in);
    CHECK_EQ(int(out[0]), 0x11);
    CHECK_EQ(int(out[1]), 0xAA);
    CHECK_EQ(int(out[2]), 0xAA);
    CHECK_EQ(int(out[3]), 0xAA);
    CHECK_EQ(int(out[4]), 0x22);
}

TEST("z80 decompression handles a truncated run marker") {
    std::uint8_t out[32];
    std::memset(out, 0, sizeof out);
    // ED at the very end with no following bytes: must not read past the end.
    const std::uint8_t in[] = {0x11, 0xED};
    z80Decompress(out, sizeof out, in, sizeof in);
    CHECK_EQ(int(out[0]), 0x11);
    CHECK_EQ(int(out[1]), 0xED);
}

TEST("z80 decompression cannot overrun its destination") {
    // The original's guard was a fixed `out < 0x10000`, so a run list that
    // expanded past the 16 KiB page it was decompressing into overran the
    // buffer.  A crafted .Z80 could reach this.
    //
    // Decompress into a page surrounded by a canary and check the canary.
    constexpr std::size_t kPage = 0x4000, kGuard = 64;
    std::vector<std::uint8_t> buf(kPage + 2 * kGuard, 0);
    std::uint8_t* const page = buf.data() + kGuard;
    std::fill(buf.begin(), buf.begin() + kGuard, 0x5a);
    std::fill(buf.end() - kGuard, buf.end(), 0x5a);

    std::vector<std::uint8_t> comp;
    for (int i = 0; i < 20000; ++i) comp.push_back(0xed);
    comp.push_back(0xed);
    comp.push_back(0xff);
    comp.push_back(0x41);
    z80Decompress(page, kPage, comp.data(), comp.size());

    for (std::size_t i = 0; i < kGuard; ++i) {
        CHECK_EQ(int(buf[i]), 0x5a);
        CHECK_EQ(int(buf[kGuard + kPage + i]), 0x5a);
    }
    // The run list is longer than the page, so the page is completely filled.
    bool allFilled = true;
    for (std::size_t i = 0; i < kPage; ++i) allFilled = allFilled && page[i] != 0;
    CHECK(allFilled);

    // A truncated run marker at the very end must not read past the input.
    std::fill(page, page + kPage, 0);
    const std::uint8_t tail[] = {0x11, 0xed, 0xed};
    z80Decompress(page, kPage, tail, sizeof tail);
    CHECK_EQ(int(page[0]), 0x11);
    CHECK_EQ(int(page[1]), 0xed);

    // A zero-capacity destination is a no-op, not a crash.
    z80Decompress(nullptr, 0, tail, sizeof tail);
}

TEST("extension detection is case insensitive") {
    CHECK(formatFromPath("A.TAP") == TapeFormat::tap);
    CHECK(formatFromPath("A.Tzx") == TapeFormat::tzx);
    CHECK(formatFromPath("dir/sub/A.SNA") == TapeFormat::sna);
    CHECK(formatFromPath("A.wav") == TapeFormat::unknown);
}

// --- .SBB and .hex success paths --------------------------------------------
// Both readers used to be exercised only by the fuzzer, which feeds random
// bytes and so only proves they do not crash. These assert that a valid file
// is actually parsed into the expected blocks.

TEST("a valid .SBB container yields its blocks") {
    TapeImage img;
    img.read(path("test.sbb"));
    CHECK_EQ(img.blockCount(), 2);
    CHECK(img.errors().empty());
    // clearN and usrN come from the 48 byte container header.
    CHECK_EQ(img.clearN(), 0xBDB8);
    CHECK_EQ(img.usrN(), 0x4900);
    CHECK_EQ(img.blocks()[0].startAddress, 0x4000);
    CHECK_EQ(img.blocks()[1].startAddress, 0x8000);
    CHECK_EQ(img.blocks()[0].length, 0x4000);
    CHECK(img.blocks()[0].selected);
    CHECK(img.blocks()[0].data[1] == 7);   // the generator's i*7 pattern
}

TEST("a valid .hex dump is decoded to 0x4000 and loaded") {
    TapeImage img;
    img.read(path("test.hex"));
    CHECK_EQ(img.blockCount(), 1);
    CHECK(img.errors().empty());
    CHECK_EQ(img.blocks()[0].startAddress, 0x4000);
    CHECK_EQ(img.blocks()[0].length, 0x1800);
    CHECK_EQ(img.clearN(), 60000);
    CHECK_EQ(img.usrN(), 7997);
    // "00 03 06 09 ..." -- the generator's i*7+3 pattern.
    CHECK_EQ(int(img.blocks()[0].data[0]), 0x03);
    CHECK_EQ(int(img.blocks()[0].data[1]), 0x0A);
    CHECK_EQ(int(img.blocks()[0].data[2]), 0x11);
}

TEST("a .hex file with a non-hex digit is rejected, not read as garbage") {
    const std::string p = "/tmp/k7zx-bad.hex";
    {
        std::ofstream f(p, std::ios::binary);
        std::string body(0x1800 * 2, '0');
        body[0] = 'Z';
        f << body;
    }
    TapeImage img;
    const ReadError err = img.read(p);
    CHECK(err == ReadError::format);
    CHECK(!img.errors().empty());
    std::remove(p.c_str());
}

// ---------------------------------------------------------------------------
// Malformed-input regressions
//
// Every case below used to read or write outside the buffer it was given, and
// two of them crashed the process outright.  They are written as literal bytes
// so the shape of each malformation is visible; the TAP block length includes
// the flag byte, so a header block is 18 and a payload block is len + 1.
// ---------------------------------------------------------------------------
namespace {

std::vector<std::uint8_t> tapHeader(std::uint8_t type, std::uint16_t declared,
                                    std::uint16_t p1, std::uint16_t p2 = 0) {
    auto word = [](std::vector<std::uint8_t>& to, std::uint16_t v) {
        to.push_back(static_cast<std::uint8_t>(v & 0xff));
        to.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    // A header block: type(1) name(10) length(2) param1(2) param2(2), and the
    // two-byte block length counts the flag byte as well.
    std::vector<std::uint8_t> hdr;
    hdr.push_back(type);
    hdr.insert(hdr.end(), 10, 0);  // name
    word(hdr, declared);
    word(hdr, p1);
    word(hdr, p2);
    std::vector<std::uint8_t> out;
    word(out, static_cast<std::uint16_t>(hdr.size() + 1));
    out.push_back(0x00);  // flag: header
    out.insert(out.end(), hdr.begin(), hdr.end());
    return out;
}

std::vector<std::uint8_t> tapData(std::size_t n, std::uint8_t fill = 0) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>((n + 1) & 0xff));
    out.push_back(static_cast<std::uint8_t>(((n + 1) >> 8) & 0xff));
    out.push_back(0xff);  // flag
    out.insert(out.end(), n, fill);
    return out;
}

std::vector<std::uint8_t> join(std::vector<std::uint8_t> a, const std::vector<std::uint8_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

/// Must not crash, hang, or read outside the file.  The image is filled in
/// through a reference: BlockInfo::data points into the image's own buffer, so
/// copying a TapeImage around leaves those pointers aimed at the old one.
void loadMalformed(TapeImage& img, const std::string& p,
                   const std::vector<std::uint8_t>& bytes) {
    writeFile(p, bytes);
    img.read(p);
}

}  // namespace

TEST("a header that claims more payload than the data block holds is clamped") {
    // The header says 0xB000 bytes; the data block supplies 16.  The BASIC
    // scanner used to walk 0xB000 bytes past the end of the file.
    const std::string p = "/tmp/k7zx-mal-len.tap";
    TapeImage img;
    loadMalformed(img, p, join(tapHeader(0, 0xB000, 10), tapData(16)));
    CHECK_MSG(img.warnings().find("length exceeds") != std::string::npos, img.warnings());
    CHECK_EQ(img.blockCount(), 1u);
    // And the length it kept is the data block's, not the header's.
    CHECK_EQ(img.blocks()[0].length, 16u);
}

TEST("a BASIC program too large to leave room for the loader stub is truncated") {
    // 45 000 bytes of BASIC leaves no room below $FFFF for the 52-byte stub the
    // converter writes immediately after the program.
    const std::string p = "/tmp/k7zx-mal-bigbasic.tap";
    TapeImage probe;
    loadMalformed(probe, p, join(tapHeader(0, 45000, 10), tapData(45000)));
    Settings s;
    s.method = kFsk;
    s.samplesPerBit = kS5_00;
    s.scheme = kOneBlock;
    Converter c;
    CHECK(c.load(p));
    c.applySettings(s);
    // The point is that this returns at all: it used to write past memory_.
    c.convert("/tmp/k7zx-mal-bigbasic.wav", false);
}

TEST("a header block with no data block yields no usable block") {
    // The reader set selected = true before the data block arrived, so the
    // converter dereferenced a null data pointer and the process died.
    const std::string p = "/tmp/k7zx-mal-hdronly.tap";
    TapeImage img;
    loadMalformed(img, p, tapHeader(3, 100, 0x8000));
    CHECK_EQ(img.blockCount(), 0u);
    for (std::size_t i = 0; i < img.blocks().size(); ++i)
        CHECK(img.blocks()[i].data == nullptr || !img.blocks()[i].selected);
    Settings s;
    s.method = kFsk;
    s.samplesPerBit = kS5_00;
    Converter c;
    CHECK(c.load(p));
    c.applySettings(s);
    c.convert("/tmp/k7zx-mal-hdronly.wav", false);
}

TEST("a block type that overflows the type field is refused, not read as a terminator") {
    // `z.type` is a signed char holding '0' + type, so 0xC3 wraps negative and
    // isTerminator() reported true, dropping every later block in the file.
    // Types the code does not otherwise understand are still code blocks -- only
    // the bytes that overflow are rejected.
    const std::string p = "/tmp/k7zx-mal-type.tap";
    std::vector<std::uint8_t> bytes = tapHeader(static_cast<std::uint8_t>(0xC3), 100, 0x8000);
    const std::vector<std::uint8_t> second = tapData(64, 0xAA);
    bytes = join(bytes, second);
    TapeImage img;
    loadMalformed(img, p, bytes);
    CHECK_MSG(img.warnings().find("unusable block type") != std::string::npos, img.warnings());

    // A type the code does not understand is not an error: 4.3 treated anything
    // that was not BASIC as a code block, and so does this.
    const std::string p2 = "/tmp/k7zx-mal-type2.tap";
    TapeImage ok;
    loadMalformed(ok, p2, join(tapHeader(0x09, 64, 0x8000), tapData(64, 0x5A)));
    CHECK_EQ(ok.blockCount(), 1u);
}

TEST("a load address above $ff3c is clamped instead of wrapping the length") {
    // kMaxAddress - 0xfff0 wraps to a *larger* length, so the clamp made the
    // block longer and the render ran off the end of the 64 KiB image.
    for (const Method m : {kManchester, kShavingsRaudo, kMilks}) {
        const std::string p = "/tmp/k7zx-mal-addr.tap";
        std::vector<std::uint8_t> bytes =
            join(tapHeader(3, 1024, 0xFFF0), tapData(1024, 0x5A));
        TapeImage probe;
        loadMalformed(probe, p, bytes);
        Settings s;
        s.method = m;
        s.samplesPerBit = Settings::allowedSamplesPerBit(m).front();
        s.scheme = kManyBlocks;
        Converter c;
        CHECK(c.load(p));
        c.applySettings(s);
        c.convert("/tmp/k7zx-mal-addr.wav", false);
    }
}

TEST("every truncated TZX block is refused rather than looping for ever") {
    // These nine block ids used to `break` out of the switch instead of the
    // loop, leaving the offset unchanged: the tool hung on an 11-byte file.
    const std::vector<std::uint8_t> ids = {0x21, 0x26, 0x28, 0x30, 0x31,
                                            0x32, 0x33, 0x35, 0x40};
    for (const std::uint8_t id : ids) {
        const std::string p = "/tmp/k7zx-mal-tzx.tzx";
        std::vector<std::uint8_t> bytes = {'Z', 'X', 'T', 'a', 'p', 'e', '!',
                                            0x1a, 0x01, 0x14, id};
        TapeImage img;
        loadMalformed(img, p, bytes);
        CHECK(img.errors().find("truncated") != std::string::npos);
    }
}

TEST("a TZX 0x10 block too short to hold its fields is refused") {
    const std::string p = "/tmp/k7zx-mal-short10.tzx";
    std::vector<std::uint8_t> bytes = {'Z', 'X', 'T', 'a', 'p', 'e', '!',
                                        0x1a, 0x01, 0x14,
                                        0x10, 0x03, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    TapeImage img;
    loadMalformed(img, p, bytes);
    CHECK(img.errors().find("short 0x10") != std::string::npos);
}

TEST("a TZX block longer than 64 KiB is walked correctly") {
    // The three-byte length is little-endian, i.e. low16 + 0x10000 * third.
    // It was decoded as low16 + 0x100 * third, so any block of 64 KiB or more
    // desynchronised the rest of the file.
    //
    // st_block_15 is ID, tstates(2), pause(2), used_bits(1), length(3), data:
    // the length is at +6..8 and the block is 9 + len.  This used to be built
    // with the length at +3, which is where k7zx 4.3 and an earlier version of
    // this port both put it -- the test therefore agreed with the bug.
    const std::string p = "/tmp/k7zx-mal-24bit.tzx";
    std::vector<std::uint8_t> bytes = {'Z', 'X', 'T', 'a', 'p', 'e', '!',
                                        0x1a, 0x01, 0x14};
    bytes.push_back(0x15);          // direct recording
    bytes.push_back(0x9e); bytes.push_back(0x00);  // t-states per sample = 158
    bytes.push_back(0xe8); bytes.push_back(0x03);  // pause = 1000
    bytes.push_back(0x08);          // used bits in the last byte
    bytes.push_back(0x00); bytes.push_back(0x00); bytes.push_back(0x01);  // len 0x10000
    bytes.insert(bytes.end(), 0x10000, 0x00);
    bytes.push_back(0x20);          // a pause block, so there is something after
    bytes.push_back(0x00); bytes.push_back(0x00);
    TapeImage img;
    loadMalformed(img, p, bytes);
    CHECK_MSG(img.errors().empty(), img.errors());  // walked the file without complaint
}

// ---------------------------------------------------------------------------
// TZX: the 0x10 header fields and the two header lengths
// ---------------------------------------------------------------------------

namespace {

/// st_block_10 wrapping one .TAP block.  A TZX 0x10 block supplies its own
/// length, so the payload carries no .TAP length framing: flag, tipo,
/// nombre(10), longitud(2), dir_ini(2), pam2(2).  Parity bytes are left out,
/// matching the rest of this file's fixtures -- neither reader checks them.
std::vector<std::uint8_t> tzx10(const std::vector<std::uint8_t>& tapBlock) {
    std::vector<std::uint8_t> out{0x10, 0xe8, 0x03};
    out.push_back(static_cast<std::uint8_t>(tapBlock.size() & 0xff));
    out.push_back(static_cast<std::uint8_t>(tapBlock.size() >> 8));
    out.insert(out.end(), tapBlock.begin(), tapBlock.end());
    return out;
}

/// A BASIC header block as it appears inside a 0x10 block, declaring the given
/// payload length, autostart line and "vars" offset.
std::vector<std::uint8_t> basicHeader(std::uint16_t length, std::uint16_t autostart,
                                      std::uint16_t vars) {
    std::vector<std::uint8_t> h{0x00, 0x00};  // flag: header, type: BASIC
    for (const char c : std::string("HDRTEST   ")) h.push_back(static_cast<std::uint8_t>(c));
    auto w = [&h](std::uint16_t v) {
        h.push_back(static_cast<std::uint8_t>(v & 0xff));
        h.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    w(length);
    w(autostart);
    w(vars);
    return h;
}

/// "ZXTape!" 0x1A major minor, then `startId` when `withStartId` is set.
std::vector<std::uint8_t> tzxHeader(bool withStartId, std::uint16_t startId) {
    std::vector<std::uint8_t> h{'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1a, 0x01, 0x14};
    if (withStartId) {
        h.push_back(static_cast<std::uint8_t>(startId & 0xff));
        h.push_back(static_cast<std::uint8_t>(startId >> 8));
    }
    return h;
}

/// A 0x10 data block's payload: the 0xFF flag and `n` bytes, with no parity.
std::vector<std::uint8_t> tzxData(std::size_t n, std::uint8_t fill = 0x41) {
    std::vector<std::uint8_t> h{0xff};
    h.insert(h.end(), n, fill);
    return h;
}

}  // namespace

TEST("TZX: a 0x10 BASIC header reads the same fields as the .tap reader") {
    // st_block_10's payload starts at its flag byte, so longitud is at +12,
    // dir_ini at +14 and pam2 at +16.  They used to be read at +13/+15/+17, one
    // byte high, so every .tzx BASIC tape produced a loader with a garbled
    // autostart line and a garbled VARS offset.
    constexpr std::uint16_t kLen = 16, kLine = 20, kVars = 1000;
    const std::string pTap = "/tmp/k7zx-tzx10.tap";
    writeFile(pTap, join(tapHeader(0, kLen, kLine, kVars), tapData(kLen)));
    TapeImage tapImg;
    CHECK(tapImg.read(pTap) == ReadError::none);
    CHECK_EQ(tapImg.blockCount(), 1);
    CHECK_EQ(tapImg.blocks()[0].auto_run, kLine);
    CHECK_EQ(tapImg.blocks()[0].param2, kVars);

    const std::string pTzx = "/tmp/k7zx-tzx10.tzx";
    const std::vector<std::uint8_t> body =
        join(tzx10(basicHeader(kLen, kLine, kVars)), tzx10(tzxData(kLen)));
    TapeImage tzxImg;
    loadMalformed(tzxImg, pTzx, join(tzxHeader(false, 0), body));
    CHECK_MSG(tzxImg.errors().empty(), tzxImg.errors());
    CHECK_EQ(tzxImg.blockCount(), 1);
    if (tzxImg.blockCount() == 1) {
        const BlockInfo& z = tzxImg.blocks()[0];
        CHECK_EQ(z.type, static_cast<std::int8_t>('0'));
        CHECK_EQ(z.length, kLen);
        CHECK_EQ(z.auto_run, kLine);  // the BASIC line to start at
        CHECK_EQ(z.param2, kVars);    // the program's "vars" offset
        // Byte for byte the same answer the .tap reader gives above.
        CHECK_EQ(z.length, tapImg.blocks()[0].length);
        CHECK_EQ(z.auto_run, tapImg.blocks()[0].auto_run);
        CHECK_EQ(z.param2, tapImg.blocks()[0].param2);
    }
    std::remove(pTap.c_str());
    std::remove(pTzx.c_str());
}

TEST("TZX: both the 10 byte and the 12 byte header are accepted") {
    // k7zx 4.3 parsed from byte 10 and k7zx's own TZX writer emits a 10 byte
    // header; the published format has a two byte start block ID there, so a
    // file from any other tool could not be read at all.  Both must work --
    // including when the start ID is 0x0010, which is also a legal block id.
    const std::vector<std::uint8_t> body =
        join(tzx10(basicHeader(16, 20, 1000)), tzx10(tzxData(16)));
    struct Case { const char* name; bool withStartId; std::uint16_t startId; };
    const Case cases[] = {{"no start id at all", false, 0},
                          {"start id 0x0000", true, 0x0000},
                          {"start id 0x0010", true, 0x0010},
                          {"start id 0x0014", true, 0x0014}};
    for (const Case& c : cases) {
        const std::string p = "/tmp/k7zx-tzxhead.tzx";
        TapeImage img;
        loadMalformed(img, p, join(tzxHeader(c.withStartId, c.startId), body));
        const std::string tag = std::string(c.name) + ": ";
        CHECK_MSG(img.errors().empty(), tag + img.errors());
        CHECK_MSG(img.blockCount() == 1, tag + "no block found");
        if (img.blockCount() == 1) {
            CHECK_MSG(img.blocks()[0].length == 16, tag + "wrong length");
            CHECK_MSG(img.blocks()[0].auto_run == 20, tag + "wrong autostart line");
            CHECK_MSG(img.blocks()[0].param2 == 1000, tag + "wrong vars offset");
        }
        std::remove(p.c_str());
    }
}

TEST("Z80: a 128K file repeating a bank cannot walk off the page-swap hook table") {
    // The 128K layout has seven page slots, and multi_128 lays its hooks out at
    // 3 + 20 + slot * 5 in a 76 byte table.  `slot` was bounded only by
    // kMaxBlocks, so a file repeating one bank -- which the format allows --
    // reached slot 13 and wrote at index 89, over the BASIC stub that follows
    // the table in memory.
    std::vector<std::uint8_t> b(30);      // 30 byte v2/v3 header
    b.push_back(20); b.push_back(0);      // extra header length
    b.resize(52, 0);                      // extra header, bytes 32..51
    b[34] = 4;                            // hardware mode -> 128K
    b[35] = 5;                            // the page currently in $C000
    for (int i = 0; i < 12; ++i) {        // twelve copies of "bank 0"
        b.push_back(0x04); b.push_back(0x00);  // compressed length 4
        b.push_back(0x03);                     // page 3 -> bank 0
        b.push_back(0xED); b.push_back(0xED); b.push_back(0x03); b.push_back(0x00);
    }
    const std::string p = "/tmp/k7zx-z80-pages.z80";
    TapeImage img;
    loadMalformed(img, p, b);
    // Seven blocks, and entry [blockCount] is still the terminator the block
    // list documents: the surplus pages were dropped, not written anywhere.
    CHECK_EQ(img.blockCount(), static_cast<int>(TapeImage::kMulti128Slots));
    CHECK(img.blocks()[TapeImage::kMulti128Slots].isTerminator());
    CHECK_MSG(img.warnings().find("more than 7 memory pages") != std::string::npos,
              img.warnings());
    // And the hook table really does have room for the seven slots the loader
    // reads, so the bound above cannot drift away from the table's size.
    CHECK(TapeImage::kMulti128Size > 3 + 21 + (TapeImage::kMulti128Slots - 1) * 5);
    std::remove(p.c_str());
}

TEST("TZX: k7zx's own --emulate output is a file it can read back") {
    // The writer and the reader have to agree on the header length.  They did
    // not: the writer emitted the 10 bytes k7zx 4.3's struct was, the reader
    // skipped the 12 the format defines, and neither could read a .tzx from any
    // other tool.
    const std::string p = "/tmp/k7zx-emulate-roundtrip.tzx";
    Converter c;
    CHECK(c.load(path("test.tap")));
    Settings s;
    s.conversionMode = kConvertHiSpeed;
    s.method = kShavingsRaudo;
    s.samplesPerBit = kS2_75;
    s.sampleRate = 48000;
    s.scheme = kManyBlocks;
    s.waveform = kCubic;
    c.applySettings(s);
    c.audioOptions().emulate = true;
    CHECK(c.convert(p, false).ok);

    std::ifstream in(p, std::ios::binary);
    const std::vector<std::uint8_t> d((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
    CHECK(d.size() > 12);
    CHECK_EQ(std::string(reinterpret_cast<const char*>(d.data()), 7), std::string("ZXTape!"));
    CHECK_EQ(int(d[7]), 0x1a);
    const ByteReader r(d);
    CHECK_EQ(r.le16(10), 0x0030u);  // the ID of the first block, which is at 12
    CHECK_EQ(int(d[12]), 0x30);
    std::remove(p.c_str());
}
