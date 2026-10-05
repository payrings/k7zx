// k7zx 5.0 - modern C++ port
//
// The LZ compression used by the Rayo loader (added by this port; not part
// of k7zx 4.3).  The PC compresses, and a 76-byte Z80 routine in the loader
// expands the data in place once the tape has loaded -- in place, because a
// Spectrum has no spare buffer and the expansion has to happen over the
// compressed data itself.  See inPlaceMargin() and README, "The Rayo loader".
//
// Format (bits MSB first; a bit-buffer byte is taken from the byte stream at
// the moment its first bit is needed, so bits and bytes interleave):
//
//     stream   := literal { token } end
//     literal  := gamma(n) <n bytes>                      n >= 1
//     after a literal:  0 -> repmatch,  1 -> newmatch
//     after a match:    0 -> literal,   1 -> newmatch
//     repmatch := gamma(n)                copy n bytes from the last offset
//     newmatch := gamma(hi+1) <lo> gamma(n-1)    offset = hi*256 + lo + 1, n >= 2
//     end      := 1 gamma(256)
//
// gamma(v), v >= 1, is interlaced Elias gamma: for each bit of v after its
// leading 1, MSB first, a 0 then that bit; then a final 1.
#ifndef K7ZX_LZ_H
#define K7ZX_LZ_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace k7zx::lz {

/// Compress `n` bytes (near-optimal parse).
std::vector<std::uint8_t> compress(const std::uint8_t* data, std::size_t n);

/// Reference decompressor.  Returns false on a malformed stream.  If
/// `reads` is given, it receives, for every output byte, how many input
/// bytes had been consumed when that byte was written.
bool decompress(const std::uint8_t* comp, std::size_t clen, std::vector<std::uint8_t>& out,
                std::vector<std::size_t>* reads = nullptr, std::size_t maxOut = 0x10000);

/// The smallest d such that the compressed stream, placed so that it ends d
/// bytes past the end of the destination ([dst+N+d-C, dst+N+d)), expands to
/// [dst, dst+N) without overwriting input it has not read yet.  -1 if the
/// stream does not decompress to `data`.
int inPlaceMargin(const std::uint8_t* data, std::size_t n, const std::vector<std::uint8_t>& comp);

}  // namespace k7zx::lz

#endif  // K7ZX_LZ_H
