// k7zx 5.0 - modern C++ port
//
// LZ compressor / reference decompressor for the Rayo loader.  See lz.h.
#include "lz.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace k7zx::lz {
namespace {

constexpr std::uint32_t kInf = std::numeric_limits<std::uint32_t>::max() / 2;
constexpr int kMaxChain = 256;          // hash-chain candidates looked at per position
constexpr std::size_t kMaxLen = 65535;  // longest match / literal run

std::uint32_t gammaBits(std::size_t v) {
    int b = 0;
    while (v > 1) { v >>= 1; ++b; }
    return static_cast<std::uint32_t>(2 * b + 1);
}

class BitWriter {
public:
    void bit(int b) {
        if (mask_ == 0) {
            bpos_ = out_.size();
            out_.push_back(0);
            mask_ = 0x80;
        }
        if (b) out_[bpos_] |= mask_;
        mask_ >>= 1;
    }
    void gamma(std::size_t v) {
        int top = 0;
        while ((v >> (top + 1)) != 0) ++top;
        for (int i = top - 1; i >= 0; --i) {
            bit(0);
            bit(static_cast<int>((v >> i) & 1));
        }
        bit(1);
    }
    void byte(std::uint8_t b) { out_.push_back(b); }
    std::vector<std::uint8_t> take() { return std::move(out_); }

private:
    std::vector<std::uint8_t> out_;
    std::size_t bpos_ = 0;
    std::uint8_t mask_ = 0;
};

struct Cand { std::uint32_t offset, length; };

// run[i]: how many bytes from i on equal d[i].  Lets long runs of one value
// (blank screens, zero-filled RAM) be compared in one step instead of byte by
// byte, which would make match finding quadratic in the run length.
std::vector<std::uint32_t> runLengths(const std::uint8_t* d, std::size_t n) {
    std::vector<std::uint32_t> run(n + 1, 0);
    for (std::size_t i = n; i-- > 0;)
        run[i] = (i + 1 < n && d[i + 1] == d[i]) ? run[i + 1] + 1 : 1;
    return run;
}

// Length of the common prefix of d[a..] and d[b..], at most lim.
std::size_t matchLength(const std::uint8_t* d, const std::vector<std::uint32_t>& run,
                        std::size_t a, std::size_t b, std::size_t lim) {
    std::size_t L = 0;
    while (L < lim && d[a + L] == d[b + L]) {
        const std::size_t ra = run[a + L], rb = run[b + L];
        if (ra != rb) { L += std::min(ra, rb); break; }  // one run ends first: mismatch next
        L += ra;
    }
    return std::min(L, lim);
}

// For each position, matches of strictly increasing length at increasing
// offsets: the cheapest offset for every achievable length is in the list.
std::vector<std::vector<Cand>> findMatches(const std::uint8_t* d, std::size_t n,
                                           const std::vector<std::uint32_t>& run) {
    std::vector<std::vector<Cand>> cands(n);
    std::vector<std::int32_t> head(65536, -1), prev(n, -1);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const unsigned key = d[i] | (d[i + 1] << 8);
        const std::size_t lim = std::min(n - i, kMaxLen);
        std::size_t best = 1;
        int seen = 0;
        for (std::int32_t j = head[key]; j >= 0 && seen < kMaxChain; j = prev[j], ++seen) {
            const std::size_t o = i - static_cast<std::size_t>(j);
            if (o > 0xff00) break;  // hi + 1 must stay below 256
            const std::size_t L = matchLength(d, run, static_cast<std::size_t>(j), i, lim);
            if (L > best) {
                cands[i].push_back({static_cast<std::uint32_t>(o), static_cast<std::uint32_t>(L)});
                best = L;
                if (L == lim) break;
            }
        }
        prev[i] = head[key];
        head[key] = static_cast<std::int32_t>(i);
    }
    return cands;
}

}  // namespace

std::vector<std::uint8_t> compress(const std::uint8_t* d, std::size_t n) {
    if (n == 0) return {};
    const std::vector<std::uint32_t> run = runLengths(d, n);
    const std::vector<std::vector<Cand>> cands = findMatches(d, n, run);

    // costL[i] / costM[i]: cheapest parse of d[0..i) ending in a literal run
    // / a match; offL / offM: the last offset on that path.
    std::vector<std::uint32_t> costL(n + 1, kInf), costM(n + 1, kInf);
    std::vector<std::uint32_t> offL(n + 1, 0), offM(n + 1, 0);
    std::vector<std::uint32_t> runStart(n + 1, 0);
    struct MFrom { std::uint32_t start, len, offset; bool rep, fromL; };
    std::vector<MFrom> fromM(n + 1);
    costM[0] = 0;  // the stream opens with a literal, which needs no flag

    for (std::size_t i = 0; i <= n; ++i) {
        if (i < n) {
            // start a literal run at i (after a match, or at the start)
            if (costM[i] < kInf) {
                const std::uint32_t c = costM[i] + (i > 0 ? 1u : 0u) + gammaBits(1) + 8;
                if (c < costL[i + 1]) {
                    costL[i + 1] = c; offL[i + 1] = offM[i]; runStart[i + 1] = static_cast<std::uint32_t>(i);
                }
            }
            // extend the run that ends at i
            if (costL[i] < kInf && i - runStart[i] < kMaxLen) {
                const std::size_t r = i - runStart[i];
                const std::uint32_t c = costL[i] + 8 + gammaBits(r + 1) - gammaBits(r);
                if (c < costL[i + 1]) {
                    costL[i + 1] = c; offL[i + 1] = offL[i]; runStart[i + 1] = runStart[i];
                }
            }
        }
        if (i >= n) break;

        // repeat the last offset: only allowed straight after a literal
        if (costL[i] < kInf && offL[i] > 0) {
            const std::size_t o = offL[i];
            const std::size_t L = matchLength(d, run, i - o, i, std::min(n - i, kMaxLen));
            for (std::size_t k : {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4}, L}) {
                if (k < 1 || k > L) continue;
                const std::uint32_t c = costL[i] + 1 + gammaBits(k);
                if (c < costM[i + k]) {
                    costM[i + k] = c; offM[i + k] = static_cast<std::uint32_t>(o);
                    fromM[i + k] = {static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(k),
                                    static_cast<std::uint32_t>(o), true, true};
                }
            }
        }

        // a new offset
        const bool fromL = costL[i] <= costM[i];
        const std::uint32_t base = fromL ? costL[i] : costM[i];
        if (base >= kInf) continue;
        std::size_t prevLen = 1;
        for (const Cand& cd : cands[i]) {
            const std::size_t hi = (cd.offset - 1) >> 8;
            const std::uint32_t oc = base + 1 + gammaBits(hi + 1) + 8;
            auto relax = [&](std::size_t k) {
                const std::uint32_t c = oc + gammaBits(k - 1);
                if (c < costM[i + k]) {
                    costM[i + k] = c; offM[i + k] = cd.offset;
                    fromM[i + k] = {static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(k), cd.offset,
                                    false, fromL};
                }
            };
            // lengths this offset is the cheapest for: (prevLen, cd.length]
            const std::size_t lo = std::max<std::size_t>(2, prevLen + 1);
            for (std::size_t k = lo; k <= cd.length && k < lo + 16; ++k) relax(k);
            for (std::size_t k = 32; k < cd.length; k *= 2)
                if (k >= lo) relax(k);
            relax(cd.length);
            prevLen = cd.length;
        }
    }

    // --- backtrack -----------------------------------------------------------
    struct Tok { char kind; std::uint32_t start, len, offset; };
    std::vector<Tok> toks;
    std::size_t i = n;
    bool inL = costL[n] <= costM[n];
    while (i > 0) {
        if (inL) {
            const std::size_t s = runStart[i];
            toks.push_back({'l', static_cast<std::uint32_t>(s), static_cast<std::uint32_t>(i - s), 0});
            i = s;
            inL = false;
        } else {
            const MFrom& f = fromM[i];
            toks.push_back({f.rep ? 'r' : 'n', f.start, f.len, f.offset});
            i = f.start;
            inL = f.fromL;
        }
    }
    std::reverse(toks.begin(), toks.end());

    // --- emit ------------------------------------------------------------------
    BitWriter w;
    bool first = true;
    for (const Tok& t : toks) {
        if (t.kind == 'l') {
            if (!first) w.bit(0);
            w.gamma(t.len);
            for (std::size_t k = 0; k < t.len; ++k) w.byte(d[t.start + k]);
        } else if (t.kind == 'r') {
            w.bit(0);
            w.gamma(t.len);
        } else {
            w.bit(1);
            const std::size_t o = t.offset - 1;
            w.gamma((o >> 8) + 1);
            w.byte(static_cast<std::uint8_t>(o & 0xff));
            w.gamma(t.len - 1);
        }
        first = false;
    }
    w.bit(1);
    w.gamma(256);
    return w.take();
}

bool decompress(const std::uint8_t* c, std::size_t clen, std::vector<std::uint8_t>& out,
                std::vector<std::size_t>* reads, std::size_t maxOut) {
    out.clear();
    if (reads) reads->clear();
    std::size_t pos = 0;
    std::uint8_t buf = 0, mask = 0;
    bool bad = false;
    auto rbyte = [&]() -> std::uint8_t {
        if (pos >= clen) { bad = true; return 0; }
        return c[pos++];
    };
    auto bit = [&]() -> int {
        if (mask == 0) { buf = rbyte(); mask = 0x80; }
        const int b = (buf & mask) ? 1 : 0;
        mask >>= 1;
        return b;
    };
    auto gamma = [&]() -> std::size_t {
        std::size_t v = 1;
        while (!bit()) {
            v = (v << 1) | static_cast<std::size_t>(bit());
            if (bad || v > 0x1ffff) { bad = true; return 1; }
        }
        return v;
    };
    auto put = [&](std::uint8_t b) {
        out.push_back(b);
        if (reads) reads->push_back(pos);
    };
    std::size_t last = 0;
    std::size_t len = gamma();
    for (std::size_t k = 0; k < len && !bad; ++k) put(rbyte());
    bool afterLiteral = true;
    while (!bad) {
        if (out.size() > maxOut) return false;
        if (bit()) {
            const std::size_t h = gamma();
            if (h == 256) break;
            if (h > 256) return false;
            last = (((h - 1) << 8) | rbyte()) + 1;
            len = gamma() + 1;
            if (last > out.size()) return false;
            for (std::size_t k = 0; k < len; ++k) put(out[out.size() - last]);
            afterLiteral = false;
        } else if (afterLiteral) {
            len = gamma();
            if (last == 0 || last > out.size()) return false;
            for (std::size_t k = 0; k < len; ++k) put(out[out.size() - last]);
            afterLiteral = false;
        } else {
            len = gamma();
            for (std::size_t k = 0; k < len && !bad; ++k) put(rbyte());
            afterLiteral = true;
        }
    }
    return !bad;
}

int inPlaceMargin(const std::uint8_t* data, std::size_t n, const std::vector<std::uint8_t>& comp) {
    std::vector<std::uint8_t> out;
    std::vector<std::size_t> reads;
    if (!decompress(comp.data(), comp.size(), out, &reads) || out.size() != n ||
        !std::equal(out.begin(), out.end(), data))
        return -1;
    long d = 0;
    const long N = static_cast<long>(n), C = static_cast<long>(comp.size());
    for (std::size_t o = 0; o < reads.size(); ++o)
        d = std::max(d, static_cast<long>(o) - static_cast<long>(reads[o]) - N + C + 1);
    return static_cast<int>(d);
}

}  // namespace k7zx::lz
