// k7zx 5.0 - modern C++ port
//
// Tape image readers: .TAP, .TZX, .SNA, .Z80, .SBB and .HEX.
//
// The original (zxfiles.cpp) parsed each format by casting the raw file buffer
// to packed structs and walking it with hand-computed offsets.  That is kept
// here, but every multi-byte field goes through the bounds-checked little
// endian readers in byteorder.h so that a truncated or hostile tape file
// produces a diagnostic instead of undefined behaviour.
#ifndef K7ZX_ZXFILES_H
#define K7ZX_ZXFILES_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "byteorder.h"
#include "defs.h"

namespace k7zx {

/// Error codes returned by TapeImage::read.  Same numbering as the original's
/// E_* constants in zxfiles.h.
enum class ReadError : int {
    none = 0,
    open = -1,      // E_ABRIR
    write = -2,     // E_ESCRIBIR
    io = -3,        // E_LEER
    version = -4,   // E_VERSION
    format = -5,    // E_FORMATO
    clear = -6,     // E_CLEAR
    usr = -7,       // E_USR
    nonsense = -8,  // E_NONSENSE
    memory = -9,    // E_MEMORIA
    thrown = -10    // std::exception from a reader
};

const char* readErrorText(ReadError e);

/// One tape block.  `data` points into the owning TapeImage (either into the
/// raw file buffer for TAP/TZX, or into one of the decompressed pages for
/// SNA/Z80) and stays valid for the lifetime of the image.
struct BlockInfo {
    bool selected = false;   ///< user selected this block for conversion
    std::int8_t type = -1;   ///< '0'..'8' per TZX header type, -1 = terminator
    std::uint16_t startAddress = 0;   ///< load address
    std::uint16_t length = 0; ///< payload length
    std::uint16_t auto_run = 0; ///< BASIC autostart line
    std::uint16_t param2 = 0;    ///< BASIC "vars" offset
    std::uint16_t execAddress = 0;  ///< entry point
    const std::uint8_t* data = nullptr;

    /// Does this block end the list?
    bool isTerminator() const { return type < 0; }
};

/// Snapshot register state, recovered from .SNA / .Z80 headers.
struct SnapData {
    std::int8_t snapshotType = 0;  ///< 0 none, 1 = 48K, 2 = 128K
    std::int8_t page = 0;     ///< currently paged-in RAM bank
    std::uint16_t HLx = 0, DEx = 0, BCx = 0, AFx = 0;
    std::uint16_t IY = 0, IX = 0, HL = 0, DE = 0;
    std::uint16_t imDiBorder = 0;
    std::uint16_t IR = 0, BC = 0, AF = 0, SP = 0;
    std::int8_t jp_reti = 0;  ///< 0xED for RETI, 0xC3 for JP
    std::uint16_t PC = 0;
};

/// A parsed tape / snapshot image plus the scratch memory the converter needs.
class TapeImage {
public:
    static constexpr std::size_t kMaxBlocks = 32;
    static constexpr std::size_t kPageSize = 0x4000;
    static constexpr std::size_t kMemorySize = 64 * 1024;

    TapeImage();

    /// Load and parse `path`.  Returns ReadError::none on success.  On failure
    /// the object is left in a clean, empty state and `errors()` explains why.
    ReadError read(const std::string& path);

    /// The raw file bytes (empty when read() failed).
    const std::vector<std::uint8_t>& buffer() const { return buffer_; }

    /// Blocks found in the image.  Entry [blockCount] is always a terminator.
    const std::array<BlockInfo, kMaxBlocks>& blocks() const { return blocks_; }
    std::array<BlockInfo, kMaxBlocks>& blocks() { return blocks_; }
    int blockCount() const { return blockCount_; }

    const SnapData& snap() const { return snap_; }

    /// Spectrum CLEAR value recovered from the image, or user supplied.
    unsigned clearN() const { return clearN_; }
    /// USR entry point recovered from the image, or user supplied.
    unsigned usrN() const { return usrN_; }

    /// Set by the user through the GUI.
    void setClearN(unsigned v) { clearN_ = v; }
    void setUsrN(unsigned v) { usrN_ = v; }

    const std::string& prgName() const { return prgName_; }
    /// The 10 name bytes exactly as they appeared in the tape header the
    /// program name came from (empty if it did not come from one).  k7zx 4.3
    /// copied these verbatim into the loader's own header.
    const std::string& rawHeaderName() const { return rawHeaderName_; }
    void setPrgName(const std::string& n) { prgName_ = n.substr(0, 10); }

    /// True for SBB images made of explicit 48K/128K snapshot pages.
    bool snapshotBlocks() const { return snapshotBlocks_; }

    const std::string& warnings() const { return warnings_; }
    const std::string& errors() const { return errors_; }

    /// 64 KiB working memory the loader routines are assembled into.
    std::array<std::uint8_t, kMemorySize>& memory() { return memory_; }
    const std::array<std::uint8_t, kMemorySize>& memory() const { return memory_; }

    /// Decompressed 16 KiB RAM pages, indexed 0..7.
    std::array<std::array<std::uint8_t, kPageSize>, 8>& pages() { return pages_; }

    /// Patch bytes applied to the assembled memory image before rendering.
    void addPoke(std::uint16_t addr, std::uint8_t value);
    void clearPokes() { pokes_.clear(); }
    void applyPokes();

    /// Number of page-swap hook bytes for 128K snapshots (multi_128 in the
    /// original).  The hooks for one page slot occupy five bytes starting at
    /// 3 + 20, so this also fixes how many slots the table can describe.
    static constexpr std::size_t kMulti128Size = 76;
    static constexpr int kMulti128Slots = 7;  ///< slots the 128K design has

    /// Page-swap hook bytes for 128K snapshots (multi_128 in the original).
    std::array<std::uint8_t, kMulti128Size>& multi128() { return multi128_; }
    const std::array<std::uint8_t, kMulti128Size>& multi128() const { return multi128_; }

    /// Working copy of the synthetic BASIC stub (BAS) appended after a
    /// reloaded BASIC program.  Patched with the program's vars offset,
    /// length and autostart line.
    std::array<std::uint8_t, 52>& bas() { return bas_; }

    /// Assemble the loader trampoline that restores a snapshot's registers.
    /// `m141` selects the shortened variant for the fast Raudo loader.
    void prepareSnapshotMemoryForConverter(bool m141) { prepareSnapshotMemory(m141); }
    /// 48K snapshots for a loader that occupies [base, $FFFF] (Rayo; added by
    /// this port): that RAM is parked just below the restore stub at $4900
    /// and copied back by it, exactly as the Raudo variant does for $FDBF.
    void prepareSnapshotMemoryForLoaderAt(unsigned base);

    /// Record a non-fatal diagnostic.  The parsers call warn() directly; the
    /// converter uses this when it has to shorten or drop a block itself.
    void addWarning(const std::string& msg) { warn(msg); }

private:
    void reset();
    void warn(const std::string& msg);
    void fail(const std::string& msg);

    void readHex();
    void readTap();
    void readTzx();
    /// Walk a TZX block list starting at `offset`.  Throws on a malformed
    /// block.  Split out of readTzx() so the 12 byte and the 10 byte header
    /// can both be tried without duplicating the walker.
    bool walkTzx(std::size_t offset, const ByteReader& r);
    void readSna();
    void readZ80();
    void readSbb();

    /// Scan a BASIC program for CLEAR and USR values (buscaUSR).
    void scanBasicForClearAndUsr(const std::uint8_t* bc, std::size_t size);

    /// Snapshot memory fix-ups (preparamemoriasnap).
    void prepareSnapshotMemory(bool m141);

    /// Size of the tightly packed register block the snapshot trampoline at
    /// 0xc900 (or 0x4900) expects: HL' DE' BC' AF' IY IX HL DE
    /// im/DI/border IR BC AF SP jp_reti PC.
    // HLx DEx BCx AFx IY IX HL DE im_di_brdr IR BC AF SP (13 words), jp_reti,
    // PC -- the tail of the original's packed st_snap_data from HLx on.  The
    // restore stub (deco_snap) is placed right after it and entered at
    // 0x4900 + 29, so this must be 29: it was 27 (12 words), which put the stub
    // two bytes early and entered every snapshot restore on an RST $38.
    static constexpr std::size_t kRegisterBlockSize = 2 * 13 + 1 + 2;
    static_assert(kRegisterBlockSize == 29);
    const std::uint8_t* registerBlock() const;

    void terminatorAfter(int index);

    std::vector<std::uint8_t> buffer_;
    std::array<std::array<std::uint8_t, kPageSize>, 8> pages_{};
    std::array<std::uint8_t, kMemorySize> memory_{};
    std::array<BlockInfo, kMaxBlocks> blocks_{};
    int blockCount_ = 0;
    SnapData snap_{};
    unsigned clearN_ = 0;
    unsigned usrN_ = 0;
    std::string prgName_;
    std::string rawHeaderName_;
    bool snapshotBlocks_ = false;
    std::string warnings_;
    std::string errors_;
    std::array<std::uint8_t, kMulti128Size> multi128_{};
    /// Block count produced by the last successful walkTzx().
    int walkBlocks_ = 0;
    std::array<std::uint8_t, 52> bas_{};
    mutable std::array<std::uint8_t, kRegisterBlockSize + 4> registerBlock_{};
    std::array<std::uint8_t, 64> deco_{};
    std::vector<std::pair<std::uint16_t, std::uint8_t>> pokes_;
};

/// Expand a z80-style RLE-compressed block into `out`.
///
/// `outCapacity` is the number of writable bytes at `out`.  The .Z80 format
/// caps a page at 16 KiB but a v1 block at 48 KiB, and a malformed file can
/// describe a run list that expands to more than either -- the original had a
/// fixed `out < 0x10000` guard and happily ran off the end of a 16 KiB page.
void z80Decompress(std::uint8_t* out, std::size_t outCapacity, const std::uint8_t* in,
                   std::size_t compressedLen);

/// Map a file name to one of the supported extensions.
enum class TapeFormat { unknown, hex, tap, tzx, sna, z80, sbb };
TapeFormat formatFromPath(const std::string& path);

}  // namespace k7zx

#endif  // K7ZX_ZXFILES_H
