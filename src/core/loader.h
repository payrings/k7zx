// k7zx 5.0 - modern C++ port
//
// Access to the hand-assembled Z80 loader routines.
//
// Porting note: the original declared these as writable `unsigned char[]` and
// patched them in place.  They are `constexpr` here and are copied into a
// scratch buffer (`LoaderRoutine`) before patching, so the tables stay in
// .rodata and the patched state is per-conversion instead of global.
#ifndef K7ZX_LOADER_H
#define K7ZX_LOADER_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "defs.h"

namespace k7zx {

/// Index into the `codes_extra` table.
enum ExtraRoutine : int {
    kRomMini = 0,
    kFskR = 1,
    kFiMini = 2,
    kFiQFast = 3,
    kEscSlow = 4,
    kRaudo48000 = 5,
    kUltraR = 6,
    kNpuR = 7
};

namespace data {

extern const std::uint8_t* const codes[kMethodCount];
extern const std::uint8_t* const codes_extra[8];

// Helper blocks referenced by the converter.
extern const std::uint8_t multi_128[];
extern const std::uint8_t BAS[];
extern const std::uint8_t deco_snap[];
extern const std::uint8_t SYSTEM_VARS[];
extern const std::uint8_t multi_milks[];
extern const std::uint8_t multi_slow[];
extern const std::uint8_t multi_machester[];
extern const std::uint8_t load_snap_blocks[];
extern const std::uint8_t load_snap_blocks_milks[];
extern const std::uint8_t load_snap_blocks_manchester[];
extern const std::uint8_t lprint_code[];

}  // namespace data

/// Runtime size (in bytes) of each table above.  Used to copy exactly the
/// right number of bytes; the original blindly `memcpy`'d 360 bytes from
/// arrays that are all much shorter.
struct CodeSizes {
    static constexpr std::size_t rom = 100;
    static constexpr std::size_t milks = 197;
    static constexpr std::size_t fsk = 125;
    static constexpr std::size_t slow_table = 166;
    static constexpr std::size_t delta = 169;
    static constexpr std::size_t raudo = 277;
    static constexpr std::size_t ultra = 119;
    static constexpr std::size_t npu = 190;
    static constexpr std::size_t fi28 = 133;
    static constexpr std::size_t fiQ = 131;
    static constexpr std::size_t machester = 159;
    static constexpr std::size_t mandif = 159;
    static constexpr std::size_t escurridofast = 186;
    // The low-rate techniques and Veloz were removed in step 15; no sizes
    // remain for the reserved Method values 13-19.

    static constexpr std::size_t rom_mini = 116;
    static constexpr std::size_t fsk_min = 117;
    static constexpr std::size_t fi_mini = 122;
    static constexpr std::size_t FiQfast = 123;
    static constexpr std::size_t scurridoslow = 171;
    static constexpr std::size_t raudo48000 = 233;
    static constexpr std::size_t ultra_r = 123;
    static constexpr std::size_t npu_code_R = 212;

    static constexpr std::size_t multi_milks = 13;
    static constexpr std::size_t multi_slow = 50;
    static constexpr std::size_t multi_machester = 66;
    static constexpr std::size_t load_snap_blocks = 20;
    static constexpr std::size_t load_snap_blocks_milks = 14;
    static constexpr std::size_t load_snap_blocks_manchester = 24;
    static constexpr std::size_t lprint_code = 21;
    static constexpr std::size_t deco_snap = 64;
    static constexpr std::size_t SYSTEM_VARS = 203;
    static constexpr std::size_t BAS = 52;
    static constexpr std::size_t multi_128 = 76;

    /// How many bytes of a given method's primary loader are copied into the
    /// scratch buffer.
    static std::size_t primary(Method m);
    /// How many bytes of an extra (override) loader are copied.
    static std::size_t extra(ExtraRoutine r);
};

/// The mutable scratch copy of a loader routine.  The original used a 513
/// byte global zero-initialised buffer; patches address offsets up to ~277 so
/// the extra headroom is cheap insurance.
class LoaderRoutine {
public:
    static constexpr std::size_t kCapacity = 512;

    LoaderRoutine() { std::memset(buf_, 0, sizeof(buf_)); }

    void loadMethod(Method m) { load(data::codes[m], CodeSizes::primary(m)); }
    void loadExtra(ExtraRoutine r) { load(data::codes_extra[r], CodeSizes::extra(r)); }

    std::uint8_t& operator[](std::size_t i) { return buf_[i]; }
    std::uint8_t operator[](std::size_t i) const { return buf_[i]; }

    std::uint8_t* data() { return buf_; }

    /// Length of the NUL-terminated code body starting at offset 5.
    std::size_t bodyLength() const;

private:
    void load(const std::uint8_t* src, std::size_t n);

    std::uint8_t buf_[kCapacity];
};

}  // namespace k7zx

#endif  // K7ZX_LOADER_H
