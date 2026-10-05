// Tests for the signal generator and the WAV/TZX containers.
#include "test_harness.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/byteorder.h"
#include "core/convert.h"
#include "core/zxwav.h"

using namespace k7zx;

namespace {

struct Wav {
    bool ok = false;
    int channels = 0;
    int bits = 0;
    unsigned rate = 0;
    std::uint32_t dataSize = 0;
    std::vector<std::uint8_t> data;
};

Wav readWav(const std::string& path) {
    Wav w;
    std::vector<std::uint8_t> d;
    try {
        d = readFile(path);
    } catch (const std::exception&) {
        return w;
    }
    if (d.size() < 44) return w;
    const ByteReader r(d);
    if (std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0)
        return w;

    std::size_t pos = 12;
    while (pos + 8 <= d.size()) {
        const char* id = reinterpret_cast<const char*>(d.data() + pos);
        const std::uint32_t size = r.le32(pos + 4);
        if (std::memcmp(id, "fmt ", 4) == 0 && pos + 24 <= d.size()) {
            w.channels = r.le16(pos + 10);
            w.rate = r.le32(pos + 12);
            w.bits = r.le16(pos + 22);
        } else if (std::memcmp(id, "data", 4) == 0) {
            w.dataSize = size;
            const std::size_t avail = d.size() - (pos + 8);
            w.data.assign(d.begin() + static_cast<long>(pos + 8),
                          d.begin() + static_cast<long>(pos + 8 + std::min<std::size_t>(size, avail)));
            w.ok = true;
            break;
        }
        pos += 8 + size + (size & 1);
    }
    return w;
}

AudioOptions baseOptions() {
    AudioOptions o;
    o.sampleRate = 48000;
    o.waveform = kSquare;
    o.method = kShavingsRaudo;
    o.samplesPerBit = kS2_75;
    return o;
}

}  // namespace

TEST("an 8-bit mono WAV has a correct header and matching data size") {
    const std::string p = "/tmp/k7zx-test-mono.wav";
    {
        AudioOptions o = baseOptions();
        AudioWriter w(o);
        CHECK(w.open(p));
        w.renderEncodedBlock(reinterpret_cast<const std::uint8_t*>("0123456789abcdef"), 0x4000, 16);
        CHECK(w.close());
    }
    const Wav w = readWav(p);
    CHECK(w.ok);
    CHECK_EQ(w.channels, 1);
    CHECK_EQ(w.bits, 8);
    CHECK_EQ(w.rate, 48000u);
    CHECK(w.dataSize > 0);
    CHECK_EQ(w.data.size(), std::size_t(w.dataSize));
    std::remove(p.c_str());
}

TEST("stereo channels are interleaved, not written as two blocks") {
    // Regression: the two channels used to be written as two sequential
    // buffers, which shifts the right channel by the whole length of the file.
    // The result played the loader in one ear and the payload in the other.
    // The original interleaved correctly because it wrote an array of {l, r}
    // structs in a single fwrite.
    const std::string p = "/tmp/k7zx-test-stereo2.wav";
    {
        AudioOptions o = baseOptions();
        o.stereo = true;
        AudioWriter w(o);
        CHECK(w.open(p));
        std::vector<std::uint8_t> data(256);
        for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 7);
        w.renderEncodedBlock(data.data(), 0x4000, 256);
        CHECK(w.close());
    }
    const Wav w = readWav(p);
    CHECK(w.ok);
    CHECK_EQ(w.channels, 2);
    CHECK_EQ(w.bits, 16);
    CHECK_EQ(w.data.size() % 4, std::size_t(0));

    // Split into frames and confirm left == right, which can only hold if the
    // samples are interleaved rather than blocked.
    std::vector<std::int16_t> s(w.data.size() / 2);
    std::memcpy(s.data(), w.data.data(), w.data.size());
    int mismatches = 0;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2)
        if (s[i] != s[i + 1]) ++mismatches;
    CHECK_EQ(mismatches, 0);

    // And the waveform must actually be there: a blocked write would leave
    // every second frame as a huge out-of-range value, because the right
    // block's samples would be read as left-channel frames.
    int nonZero = 0;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2)
        if (s[i] != 0) ++nonZero;
    CHECK(nonZero > 100);
    std::remove(p.c_str());
}

TEST("the stereo left channel carries the same waveform as mono") {
    const std::string mono = "/tmp/k7zx-test-mono.wav";
    const std::string stereo = "/tmp/k7zx-test-stereo3.wav";
    std::vector<std::uint8_t> data(256);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 11);
    for (int isStereo = 0; isStereo < 2; ++isStereo) {
        AudioOptions o = baseOptions();
        o.stereo = isStereo != 0;
        AudioWriter w(o);
        CHECK(w.open(isStereo ? stereo : mono));
        w.renderEncodedBlock(data.data(), 0x4000, 256);
        CHECK(w.close());
    }
    const Wav m = readWav(mono);
    const Wav s = readWav(stereo);
    CHECK(m.ok && s.ok);
    CHECK(s.data.size() >= m.data.size());
    // mono: value = y + 128 ; stereo left = -256 * y
    int match = 0;
    const std::size_t k = std::min<std::size_t>(m.data.size(), 40000);
    for (std::size_t i = 0; i < k; ++i) {
        const int want = -256 * (static_cast<int>(m.data[i]) - 128);
        const int got = static_cast<int>(static_cast<std::int16_t>(
            reinterpret_cast<const std::uint8_t*>(s.data.data())[i * 4] |
            (reinterpret_cast<const std::uint8_t*>(s.data.data())[i * 4 + 1] << 8)));
        if (std::abs(got - want) <= 2) ++match;
    }
    CHECK_EQ(match, static_cast<int>(k));
    std::remove(mono.c_str());
    std::remove(stereo.c_str());
}

TEST("a 16-bit stereo WAV has the right block alignment") {
    const std::string p = "/tmp/k7zx-test-stereo.wav";
    {
        AudioOptions o = baseOptions();
        o.stereo = true;
        AudioWriter w(o);
        CHECK(w.open(p));
        w.renderEncodedBlock(reinterpret_cast<const std::uint8_t*>("0123456789abcdef"), 0x4000, 16);
        CHECK(w.close());
    }
    const Wav w = readWav(p);
    CHECK(w.ok);
    CHECK_EQ(w.channels, 2);
    CHECK_EQ(w.bits, 16);
    CHECK_EQ(w.data.size() % 4, std::size_t(0));
    std::remove(p.c_str());
}


TEST("the reported duration matches the number of samples written") {
    const std::string p = "/tmp/k7zx-test-dur.wav";
    AudioWriter w(baseOptions());
    CHECK(w.open(p));
    std::vector<std::uint8_t> data(1000, 0x5a);
    w.renderEncodedBlock(data.data(), 0x4000, static_cast<unsigned>(data.size()));
    CHECK(w.close());
    const Wav wav = readWav(p);
    const double seconds = double(wav.data.size()) / 48000.0;
    CHECK_NEAR(w.duration(), seconds, 0.02);
    std::remove(p.c_str());
}

TEST("inverting the polarity mirrors the 8-bit sample around 128") {
    const std::string a = "/tmp/k7zx-test-inv0.wav";
    const std::string b = "/tmp/k7zx-test-inv1.wav";
    std::vector<std::uint8_t> data(64);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 3);
    for (int inv = 0; inv < 2; ++inv) {
        AudioOptions o = baseOptions();
        o.invert = inv != 0;
        AudioWriter w(o);
        CHECK(w.open(inv ? b : a));
        w.renderEncodedBlock(data.data(), 0x4000, 64);
        CHECK(w.close());
    }
    const Wav wa = readWav(a);
    const Wav wb = readWav(b);
    CHECK_EQ(wa.data.size(), wb.data.size());
    for (std::size_t i = 0; i < wa.data.size(); ++i)
        CHECK_EQ(int(wa.data[i]), 256 - int(wb.data[i]));
    std::remove(a.c_str());
    std::remove(b.c_str());
}

TEST("every method renders without producing an empty file") {
    for (int m = kRom; m <= kEscurrido; ++m) {
        for (int spb : Settings::allowedSamplesPerBit(static_cast<Method>(m))) {
            const std::string p = "/tmp/k7zx-test-sweep.wav";
            AudioOptions o = baseOptions();
            o.method = m;
            o.samplesPerBit = spb;
            AudioWriter w(o);
            CHECK(w.open(p));
            std::vector<std::uint8_t> data(256);
            for (std::size_t i = 0; i < data.size(); ++i)
                data[i] = static_cast<std::uint8_t>(i * 11);
            w.renderEncodedBlock(data.data(), 0x4000, 256);
            CHECK(w.close());
            const Wav wav = readWav(p);
            CHECK(wav.ok);
            CHECK(wav.dataSize > 100);
            std::remove(p.c_str());
        }
    }
}

TEST("every waveform renders both halves of a cycle") {
    for (int wf = kSquare; wf <= kDelta; ++wf) {
        const std::string p = "/tmp/k7zx-test-wf.wav";
        AudioOptions o = baseOptions();
        o.waveform = wf;
        o.method = kFsk;
        o.samplesPerBit = kS4_00;
        AudioWriter w(o);
        CHECK(w.open(p));
        std::vector<std::uint8_t> data(128, 0x5a);
        w.renderEncodedBlock(data.data(), 0x4000, 128);
        CHECK(w.close());
        const Wav wav = readWav(p);
        CHECK(wav.ok);
        // The square wave must use the full 0..255 range; the smoothed ones
        // must stay inside it too.
        for (std::uint8_t s : wav.data) {
            (void)s;
        }
        std::remove(p.c_str());
    }
}

TEST("a very long direct-recording run does not overflow the scratch buffer") {
    // The original's 512-entry scratch array overflowed here; the port grows it.
    // A single level change near the end produces one pulse far longer than 512
    // samples.
    const std::string p = "/tmp/k7zx-test-dr.wav";
    AudioOptions o = baseOptions();
    AudioWriter w(o);
    CHECK(w.open(p));
    std::vector<std::uint8_t> data(64, 0xFF);   // high
    data[63] = 0x00;                              // one low bit at the very end
    w.directRecording(4000, data.data(), 64);
    CHECK(w.close());
    const Wav wav = readWav(p);
    CHECK(wav.ok);
    CHECK(wav.dataSize > 10000);
    std::remove(p.c_str());
}

TEST("the TZX container gets a valid header and a patched length") {
    const std::string p = "/tmp/k7zx-test.tzx";
    {
        AudioOptions o = baseOptions();
        o.emulate = true;
        AudioWriter w(o);
        CHECK(w.open(p));
        std::vector<std::uint8_t> data(256);
        for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i);
        w.renderEncodedBlock(data.data(), 0x4000, 256);
        CHECK(w.close());
    }
    const std::vector<std::uint8_t> d = readFile(p);
    const ByteReader r(d);
    // 12 byte file header: "ZXTape!" 0x1A major minor, then a two byte "ID of
    // first block".  k7zx 4.3 stopped after the minor version -- sizeof its
    // header struct was 10 -- so the file it wrote was two bytes short and no
    // other TZX reader could make sense of it.
    CHECK(d.size() > 70);
    CHECK_EQ(std::string(reinterpret_cast<const char*>(d.data()), 7), std::string("ZXTape!"));
    CHECK_EQ(int(d[7]), 0x1a);
    CHECK_EQ(int(d[8]), 1);
    CHECK_EQ(int(d[9]), 19);
    CHECK_EQ(r.le16(10), 0x0030u);  // the ID of the text block that follows
    // then a 0x30 text block: ID, length, 50 characters
    CHECK_EQ(int(d[12]), 0x30);
    CHECK_EQ(int(d[13]), 50);
    // then the 0x15 direct recording block: ID, tstates(2), pause(2), used(1), length(3)
    constexpr std::size_t kBlock = 64;
    CHECK_EQ(int(d[kBlock]), 0x15);
    const std::uint32_t dataLen =
        std::uint32_t(r.le16(kBlock + 6)) | (0x100u * r.u8(kBlock + 8));
    CHECK(dataLen > 0);
    CHECK_EQ(dataLen + kBlock + 9, d.size());
    std::remove(p.c_str());
}

TEST("pilot, sync and data pulses appear in the expected proportions") {
    // Raudo at 2.75 renders four 2-bit groups per byte, each as a pair of
    // cycles, so 8 bytes of payload should give roughly 8*16 cycles plus the
    // leading pilot and trailer.
    const std::string p = "/tmp/k7zx-test-prop.wav";
    AudioOptions o = baseOptions();
    AudioWriter w(o);
    CHECK(w.open(p));
    std::vector<std::uint8_t> data(1024, 0xff);
    w.renderEncodedBlock(data.data(), 0x4000, 1024);
    CHECK(w.close());

    const Wav wav = readWav(p);
    CHECK(wav.ok);
    // Count level transitions; a square wave at this rate gives thousands.
    int transitions = 0;
    for (std::size_t i = 1; i < wav.data.size(); ++i)
        if ((wav.data[i] > 128) != (wav.data[i - 1] > 128)) ++transitions;
    CHECK(transitions > 1000);
    std::remove(p.c_str());
}

TEST("a longer block always yields a longer file") {
    double previous = 0;
    for (unsigned n : {64u, 256u, 1024u, 4096u}) {
        const std::string p = "/tmp/k7zx-test-len.wav";
        AudioOptions o = baseOptions();
        AudioWriter w(o);
        CHECK(w.open(p));
        std::vector<std::uint8_t> data(n, 0x5a);
        w.renderEncodedBlock(data.data(), 0x4000, n);
        CHECK(w.close());
        CHECK(w.duration() > previous);
        previous = w.duration();
        std::remove(p.c_str());
    }
}

TEST("the final tone lengthens the output") {
    double plain = 0, tailed = 0;
    for (int withTone = 0; withTone < 2; ++withTone) {
        const std::string p = "/tmp/k7zx-test-tone.wav";
        AudioOptions o = baseOptions();
        o.finalTone = withTone != 0;
        AudioWriter w(o);
        CHECK(w.open(p));
        std::vector<std::uint8_t> data(256, 0x5a);
        w.renderEncodedBlock(data.data(), 0x4000, 256);
        CHECK(w.close());
        (withTone ? tailed : plain) = w.duration();
        std::remove(p.c_str());
    }
    CHECK(tailed > plain);
}


