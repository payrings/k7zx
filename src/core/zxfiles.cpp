// k7zx 5.0 - modern C++ port
//
// Tape image readers, ported from zxfiles.cpp.
#include "zxfiles.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "byteorder.h"
#include "loader.h"

namespace k7zx {
namespace {

constexpr unsigned char kMaxBlocks = 32;

/// Terminator of a tokenised BASIC line.
constexpr std::uint8_t kCr = 0x0d;

/// `prg_name` in the original was a fixed `char[11]`.
std::string trimProgramName(const std::string& raw) {
    std::string s = raw.substr(0, 10);
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
    return s;
}

/// Hex nibble decoder used by the .HEX reader.
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    return -1;
}

std::string lowerExtension(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = name.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

std::string baseName(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

}  // namespace

const char* readErrorText(ReadError e) {
    switch (e) {
        case ReadError::none: return "ok";
        case ReadError::open: return "cannot open file";
        case ReadError::write: return "cannot write file";
        case ReadError::io: return "error reading file";
        case ReadError::version: return "unsupported version";
        case ReadError::format: return "unsupported or corrupt file format";
        case ReadError::clear: return "CLEAR address out of range";
        case ReadError::usr: return "USR address out of range";
        case ReadError::nonsense: return "nonsensical data in file";
        case ReadError::memory: return "not enough memory";
        case ReadError::thrown: return "internal error while parsing";
    }
    return "unknown error";
}

TapeFormat formatFromPath(const std::string& path) {
    const std::string ext = lowerExtension(path);
    if (ext == ".hex") return TapeFormat::hex;
    if (ext == ".tap") return TapeFormat::tap;
    if (ext == ".tzx") return TapeFormat::tzx;
    if (ext == ".sna") return TapeFormat::sna;
    if (ext == ".z80") return TapeFormat::z80;
    if (ext == ".sbb") return TapeFormat::sbb;
    return TapeFormat::unknown;
}

// ---------------------------------------------------------------------------
// Small I/O helpers
// ---------------------------------------------------------------------------
std::vector<std::uint8_t> readFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open '" + path + "'");
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        throw std::runtime_error("cannot seek '" + path + "'");
    }
    const long n = std::ftell(f);
    if (n < 0) {
        std::fclose(f);
        throw std::runtime_error("cannot size '" + path + "'");
    }
    std::rewind(f);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(n));
    if (n > 0 && std::fread(data.data(), 1, data.size(), f) != data.size()) {
        std::fclose(f);
        throw std::runtime_error("short read on '" + path + "'");
    }
    std::fclose(f);
    return data;
}

void writeFile(const std::string& path, const std::uint8_t* bytes, std::size_t size) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot create '" + path + "'");
    if (size > 0 && std::fwrite(bytes, 1, size, f) != size) {
        std::fclose(f);
        throw std::runtime_error("short write on '" + path + "'");
    }
    std::fclose(f);
}

// ---------------------------------------------------------------------------
// z80 page decompression (the "ED ED nn bbb" RLE form used by .Z80 v2/v3)
// ---------------------------------------------------------------------------
void z80Decompress(std::uint8_t* out, std::size_t outCapacity, const std::uint8_t* in,
                   std::size_t compressedLen) {
    if (!out || outCapacity == 0) return;
    // 0xFFFF is the sentinel for "an uncompressed block".
    if (compressedLen == 0xffff) {
        std::memcpy(out, in, std::min<std::size_t>(0x4000, outCapacity));
        return;
    }
    std::size_t i = 0, o = 0;
    while (i < compressedLen && o < outCapacity) {
        if (in[i] == 0xed && i + 3 < compressedLen && in[i + 1] == 0xed) {
            const unsigned run = in[i + 2];
            const std::uint8_t value = in[i + 3];
            for (unsigned k = 0; k < run && o < outCapacity; ++k) out[o++] = value;
            i += 4;
        } else {
            out[o++] = in[i++];
        }
    }
}

// ---------------------------------------------------------------------------
// TapeImage
// ---------------------------------------------------------------------------
TapeImage::TapeImage() { reset(); }

void TapeImage::reset() {
    blockCount_ = 0;
    clearN_ = 0;
    usrN_ = 0;
    prgName_.clear();
    rawHeaderName_.clear();
    snapshotBlocks_ = false;
    snap_ = SnapData{};
    warnings_.clear();
    errors_.clear();
    pokes_.clear();
    buffer_.clear();
    std::memcpy(multi128_.data(), data::multi_128, multi128_.size());
    std::memcpy(bas_.data(), data::BAS, bas_.size());
    std::memcpy(deco_.data(), data::deco_snap, deco_.size());
    std::memset(memory_.data(), 0, memory_.size());
    for (auto& p : pages_) std::memset(p.data(), 0, p.size());
    for (auto& t : blocks_) t = BlockInfo{};
    blocks_[0].type = -1;
}

void TapeImage::warn(const std::string& msg) { warnings_ += msg; }

void TapeImage::fail(const std::string& msg) { errors_ += msg; }

void TapeImage::terminatorAfter(int index) {
    if (index >= 0 && index < static_cast<int>(blocks_.size())) blocks_[index].type = -1;
    // An index past the end of the array is a bug in the caller, not something
    // to paper over: silently terminating the *last* block would hide it.
    assert(index < static_cast<int>(blocks_.size()) && "terminator index out of range");
}

void TapeImage::addPoke(std::uint16_t addr, std::uint8_t value) { pokes_.emplace_back(addr, value); }

void TapeImage::applyPokes() {
    for (const auto& [addr, value] : pokes_) memory_[addr] = value;
}

// ---------------------------------------------------------------------------
// BASIC scanner: recover CLEAR and USR from a tokenised BASIC program
// (buscaUSR in the original)
// ---------------------------------------------------------------------------
void TapeImage::scanBasicForClearAndUsr(const std::uint8_t* bc, std::size_t size) {
    // The original cleared both before every scan (original/zxfiles.cpp:
    // `clearN=0; usrN=0;`), so with several BASIC blocks on a tape only the
    // last one that actually contained CLEAR or USR survived.  Without the
    // reset a block with neither leaves the previous block's values in place,
    // which is how a CLEAR from an earlier program ends up patching the loader.
    clearN_ = 0;
    usrN_ = 0;
    std::size_t parsed = 0;
    while (parsed < size) {
        if (parsed + 4 > size) break;
        const std::uint16_t lineLen = peek16(bc, parsed + 2);
        if (parsed + 4 + lineLen > size) break;
        const std::uint8_t* line = bc + parsed + 4;
        if (lineLen == 0) break;
        if (line[lineLen - 1] != kCr) break;

        int position = 1;
        int i_n = 0;
        bool inQuotes = false;
        bool hayCLEAR = false;
        bool hayUSR = false;
        char num[7] = {0};

        for (int c = 0; c < lineLen; ++c) {
            const std::uint8_t ch = line[c];
            if (inQuotes) {
                if (ch == '"') {
                    inQuotes = false;
                } else if (ch >= '0' && ch <= '9' && i_n < 5) {
                    num[i_n++] = static_cast<char>(ch);
                    num[i_n] = 0;
                }
            } else if (ch > 127) {
                switch (ch) {
                    case 0xea:  // the original labels this REM; it is really STR$
                        c = lineLen;
                        break;
                    case 0xfd:  // CLEAR
                        if (position == 1) {
                            hayCLEAR = true;
                            num[0] = 0;
                            i_n = 0;
                        }
                        break;
                    case 0xc0:  // USR
                        if (position != 1) {
                            hayUSR = true;
                            num[0] = 0;
                            i_n = 0;
                        }
                        break;
                    default:
                        break;
                }
            } else if (ch == '"') {
                inQuotes = true;
            } else if (ch == ':' || ch == kCr || ch == 0x0e) {
                // PORTING NOTE: the original tested its `linea` (line) member: `linea->data[c] == 0x0e`
                // with the comment `/*'\r'*/`, i.e. 0x0d was clearly intended.
                // With 0x0e the branch never fired for a real line ending, so a
                // `USR nnnnn` at the end of the last statement of a line was
                // never picked up and the generated loader would RET instead of
                // jumping to the entry point.  Both bytes are accepted here.
                if (hayCLEAR && i_n == 5) clearN_ = static_cast<unsigned>(std::atoi(num));
                if (hayUSR && i_n == 5) usrN_ = static_cast<unsigned>(std::atoi(num));
                position = 0;
                hayUSR = hayCLEAR = false;
            } else if (ch >= '0' && ch <= '9' && i_n < 5) {
                num[i_n++] = static_cast<char>(ch);
                num[i_n] = 0;
            }
            ++position;
        }
        parsed += 4 + lineLen;
    }
}

// ---------------------------------------------------------------------------
// .HEX -- a hex dump of 0x1800 bytes loaded at 0x4000
// ---------------------------------------------------------------------------
void TapeImage::readHex() {
    if (buffer_.size() < 0x1800 * 2) {
        fail("truncated .hex file\r\n");
        throw std::runtime_error("truncated .hex file");
    }
    for (int x = 0; x < 0x1800; ++x) {
        const int hi = hexDigit(static_cast<char>(buffer_[static_cast<std::size_t>(x) * 2]));
        const int lo = hexDigit(static_cast<char>(buffer_[static_cast<std::size_t>(x) * 2 + 1]));
        if (hi < 0 || lo < 0) {
            fail("invalid hex digit in .hex file\r\n");
            throw std::runtime_error("invalid hex digit in .hex file");
        }
        pages_[5][static_cast<std::size_t>(x)] =
            static_cast<std::uint8_t>(0x10 * hi + lo);
    }

    clearN_ = 60000;
    usrN_ = 7997;

    BlockInfo& t = blocks_[0];
    t.selected = true;
    t.type = '3';
    t.startAddress = 0x4000;
    t.length = 0x1800;
    t.execAddress = 7997;
    t.data = pages_[5].data();
    terminatorAfter(1);
    blockCount_ = 1;
}

// ---------------------------------------------------------------------------
// .TAP -- sequence of [len][flag][data] blocks
// ---------------------------------------------------------------------------
void TapeImage::readTap() {
    const ByteReader r(buffer_);
    std::size_t offset = 0;
    int t = 0;

    while (offset < buffer_.size()) {
        if (offset + 2 > buffer_.size()) {
            warn("truncated block header at end of .tap file\r\n");
            break;
        }
        const std::uint16_t blockLen = r.le16(offset);
        if (offset + 2 + blockLen > buffer_.size()) {
            fail("block length exceeds file size in .tap file\r\n");
            throw std::runtime_error("block length exceeds file size");
        }
        const std::size_t payload = offset + 2;
        const std::uint8_t flag = r.u8(payload);
        offset += 2 + blockLen;

        if (t >= static_cast<int>(kMaxBlocks) - 1) {
            warn("more than 7 blocks\r\n");
            break;
        }

        if (flag == 0x00) {
            // Header block: type(1) name(10) length(2) param1(2) param2(2)
            if (blockLen < 18) {
                warn("short header block in .tap file\r\n");
                continue;
            }
            const std::uint8_t type = r.u8(payload + 1);
            if (type > 79) {
                // `z.type` is a signed char holding '0' + type, so a type byte
                // of 0x80 or more wraps it negative and isTerminator() then
                // reports true, silently dropping every later block in the file.
                // Only those bytes are rejected: a type the code does not
                // otherwise understand is still a code block, exactly as 4.3
                // treated it.
                warn("unusable block type in .tap file\r\n");
                continue;
            }
            BlockInfo& z = blocks_[static_cast<std::size_t>(t)];
            z.selected = true;
            z.type = static_cast<std::int8_t>('0' + type);
            z.length = r.le16(payload + 12);
            z.startAddress = r.le16(payload + 14);
            z.execAddress = 0;

            if (z.type == '0') {
                // BASIC: relocate the program and remember the autostart line.
                z.startAddress = kBasicProgram;
                z.auto_run = r.le16(payload + 14) ? r.le16(payload + 14) : 0xfffe;
                z.param2 = r.le16(payload + 16);
            } else {
                z.execAddress = 0;
            }

            if (prgName_.empty()) {
                rawHeaderName_ = r.text(payload + 2, 10);
                prgName_ = trimProgramName(rawHeaderName_);
            }
            if (static_cast<unsigned>(z.startAddress) + z.length > kMaxAddress)
                warn("block overrides address $ff3c\r\n");
        } else if (flag == 0xff) {
            if (t >= static_cast<int>(kMaxBlocks) - 1) break;
            BlockInfo& z = blocks_[static_cast<std::size_t>(t)];
            if (z.type < 0) {
                z.type = '5';
                z.startAddress = 0;
                z.length = static_cast<std::uint16_t>(blockLen - 2);
                warn("block without header\r\n");
            }
            z.data = r.ptr(payload + 1, blockLen - 1);
            // The header block declares the payload length, and nothing checked
            // it against the bytes that actually follow.  Every later user of
            // z.length -- the BASIC scanner, the render loop, the loader --
            // trusted it, so a header that lied about its length made them read
            // (and write) far past the end of the file.  The data block wins.
            if (z.length > blockLen - 1) {
                warn("block header length exceeds the data block\r\n");
                z.length = static_cast<std::uint16_t>(blockLen - 1);
            }

            if (z.type == '0') {
                scanBasicForClearAndUsr(z.data, z.length);
                z.execAddress = static_cast<std::uint16_t>(kBasicProgram + z.length + 1);
            } else if (static_cast<unsigned>(z.startAddress) <= usrN_ &&
                       usrN_ < static_cast<unsigned>(z.startAddress) + z.length) {
                z.execAddress = static_cast<std::uint16_t>(usrN_);
            }
            ++t;
            terminatorAfter(t);
        } else {
            fail("format error in .tap file format\r\n");
            throw std::runtime_error("format error in .tap file");
        }
    }
    blockCount_ = t;
    if (blockCount_ > 7) warn("more than 7 blocks\r\n");
}

// ---------------------------------------------------------------------------
// .TZX -- the TZX container
// ---------------------------------------------------------------------------
// PORTING NOTE: the original used the wrong block sizes for 0x2A, 0x2B, 0x31,
// 0x35 and 0x40, and -- because the three-byte length fields were read as
// `low16 + 0x100 * third_byte` instead of `low16 + 0x10000 * third_byte` -- it
// mis-walked any turbo, pure-data or direct-recording block of 64 KiB or more.
// A comment here once claimed the original's 0x10000 was the wrong one; that
// was backwards, and this port had inherited the error.  The offsets and
// lengths below follow the published TZX layout.
void TapeImage::readTzx() {
constexpr std::size_t kHead = 10;      // "ZXTape!"(7) 0x1A major minor
    constexpr std::size_t kHeadWithStartId = 12;  // ... plus a 2 byte start block ID
    if (buffer_.size() < kHead) {
        fail("truncated .tzx header\r\n");
        throw std::runtime_error("truncated .tzx header");
    }
    const ByteReader r(buffer_);
    // The published TZX header is 12 bytes: the ten above plus a two byte "ID
    // of first block".  k7zx 4.3 parsed from byte 10 -- it stopped at the minor
    // version because sizeof(st_head_tzx) was 10 -- and wrote its own TZX the
    // same way, so both the files it produced and the files every other tool
    // produces have to be accepted.
    //
    // Byte 10 alone cannot settle it: a start ID of 0x0010 is both a legal
    // start ID and a legal block ID.  So the published 12 byte layout is tried
    // first and k7zx's 10 bytes second, and the walk that actually turns up
    // tape blocks wins.  When neither finds any, the 12 byte result is kept:
    // such a file has no 0x10 blocks in it, so there is nothing to lose.
    auto isBlockId = [](std::uint8_t b) {
        return (b >= 0x10 && b <= 0x40) || b == 'Z';
    };
    std::size_t candidates[2];
    int nCandidates = 0;
    if (buffer_.size() >= kHeadWithStartId) {
        candidates[nCandidates++] = kHeadWithStartId;
        if (isBlockId(buffer_[kHead])) candidates[nCandidates++] = kHead;
    } else {
        candidates[nCandidates++] = kHead;  // too short to hold a start ID
    }
    struct Attempt {
        bool ok = false;
        int blocks = 0;
        std::string errors, warnings;
        std::array<BlockInfo, kMaxBlocks> found{};
    };
    Attempt best;
    for (int n = 0; n < nCandidates; ++n) {
        errors_.clear();
        warnings_.clear();
        for (auto& b : blocks_) b = BlockInfo{};
        Attempt attempt;
        try {
            if (walkTzx(candidates[n], r)) {
                attempt.ok = true;
                attempt.blocks = walkBlocks_;
                attempt.found = blocks_;
            }
        } catch (const std::exception&) {
            // fall through to the next interpretation
        }
        attempt.errors = errors_;
        attempt.warnings = warnings_;
        if (!best.ok || (attempt.ok && attempt.blocks > best.blocks)) best = attempt;
        if (attempt.ok && attempt.blocks > 0) break;  // the other layout cannot beat this
    }
    if (!best.ok) {
        errors_ = best.errors;
        warnings_ = best.warnings;
        if (errors_.empty()) fail("error in .tzx file format\r\n");
        throw std::runtime_error("unknown tzx header or block id");
    }
    blocks_ = best.found;
    errors_ = best.errors;
    warnings_ = best.warnings;
    blockCount_ = best.blocks;
    if (blockCount_ > 7) warn("more than 7 blocks\r\n");
}

// Walk the block list from `offset`.  Throws on a malformed block; returns
// false only when the very first byte is not a block id this port knows, which
// is how a wrong header length shows up.
bool TapeImage::walkTzx(std::size_t offset, const ByteReader& r) {
    int t = 0;
    // Every block must fit in what is left of the file.  The original, and an
    // earlier version of this port, used `if (!has(...)) break;` here -- but
    // `break` inside a switch arm leaves the *switch*, not the loop, so a
    // truncated block spun forever on an unchanged offset.  Nine block ids
    // could hang the tool outright.
    auto need = [&](std::size_t n, const char* what) {
        if (!r.has(offset, n)) {
            fail(what);
            throw std::runtime_error(what);
        }
    };
    blocks_[0].type = -1;
    blocks_[0].startAddress = 0;
    blocks_[0].length = 0;
    blocks_[0].execAddress = 0;
    blocks_[0].data = nullptr;

    // Advance `offset` past a block whose size is described by the fields at the
    // given relative offsets.  Returns false if the description runs off the
    // end of the file.
    auto skip = [&](std::size_t extra) {
        if (offset + extra > buffer_.size()) return false;
        offset += extra;
        return true;
    };

    while (offset < buffer_.size()) {
        if (t >= static_cast<int>(kMaxBlocks) - 1) {
            warn("more than 7 blocks\r\n");
            break;
        }
        const std::uint8_t id = buffer_[offset];

        switch (id) {
            case 0x10: {  // Standard speed data block
                if (offset + 5 > buffer_.size()) {
                    fail("truncated 0x10 block in .tzx file\r\n");
                    throw std::runtime_error("truncated 0x10 block");
                }
                const std::uint16_t tapLen = r.le16(offset + 3);
                // st_block_10 (original/zxfiles.h) is ID, pause(2), tapLen(2)
                // and then the .TAP bytes with no length framing of their own:
                // flag, tipo, nombre(10), longitud(2), dir_ini(2), pam2(2),
                // parity -- 18 bytes before the parity, 19 with it.  A data
                // block is only 1 + payload + 1.  The field reads used to sit
                // one byte too high as well, so every .tzx BASIC tape got a
                // loader starting on a garbled line number with a garbled VARS
                // offset.
                if (tapLen < 2) {
                    // Too short to hold even a flag and its parity byte.
                    fail("short 0x10 block in .tzx file\r\n");
                    throw std::runtime_error("short 0x10 block");
                }
                offset += 5 + tapLen;
                if (offset > buffer_.size()) {
                    fail("0x10 block overruns .tzx file\r\n");
                    throw std::runtime_error("0x10 block overruns file");
                }
                const std::uint8_t flag = r.u8(offset - tapLen);
                if (flag == 0x00) {
                    if (tapLen < 18) {
                        // Not a whole header block: the field reads below
                        // would run into whatever follows it.
                        fail("short 0x10 header block in .tzx file\r\n");
                        throw std::runtime_error("short 0x10 header block");
                    }
                    BlockInfo& z = blocks_[static_cast<std::size_t>(t)];
                    if (r.u8(offset - tapLen + 1) > 79) {  // see readTap()
                        warn("unusable block type in .tzx file\r\n");
                        offset = buffer_.size();
                        break;
                    }
                    z.selected = true;
                    z.type = static_cast<std::int8_t>('0' + r.u8(offset - tapLen + 1));
                    z.startAddress = r.le16(offset - tapLen + 14);
                    z.length = r.le16(offset - tapLen + 12);
                    if (z.type == '0') {
                        z.startAddress = kBasicProgram;
                        z.auto_run = r.le16(offset - tapLen + 14) ? r.le16(offset - tapLen + 14) : 0xfffe;
                        z.length = r.le16(offset - tapLen + 12);
                        z.param2 = r.le16(offset - tapLen + 16);
                        z.execAddress = 0;
                    }
                    if (prgName_.empty()) {
                        rawHeaderName_ = r.text(offset - tapLen + 2, 10);
                        prgName_ = trimProgramName(rawHeaderName_);
                    }
                    if (static_cast<unsigned>(z.startAddress) + z.length > kMaxAddress)
                        warn("block overrides address $ff3c\r\n");
                } else if (flag == 0xff) {
                    BlockInfo& z = blocks_[static_cast<std::size_t>(t)];
                    if (z.type < 0) {
                        z.type = '5';
                        z.startAddress = 0;
                        z.length = static_cast<std::uint16_t>(tapLen - 2);
                        warn("block without header\r\n");
                    }
                    z.data = r.ptr(offset - tapLen + 1, tapLen - 1);
                    // See readTap(): the data block, not the header's claim,
                    // decides how long this block is.
                    if (z.length > tapLen - 1) {
                        warn("block header length exceeds the data block\r\n");
                        z.length = static_cast<std::uint16_t>(tapLen - 1);
                    }
                    if (z.type == '0') {
                        scanBasicForClearAndUsr(z.data, z.length);
                        // The original patched the BASIC stub that the
                        // multi-block scheme appends after the program.
                        poke16(bas_.data() + 4, 0,
                               static_cast<std::uint16_t>(kBasicProgram + z.param2));
                        poke16(bas_.data() + 10, 0,
                               static_cast<std::uint16_t>(kBasicProgram + z.length + 1));
                        poke16(bas_.data() + 38, 0, z.auto_run);
                        z.execAddress = static_cast<std::uint16_t>(kBasicProgram + z.length);
                    } else if (static_cast<unsigned>(z.startAddress) <= usrN_ &&
                               usrN_ < static_cast<unsigned>(z.startAddress) + z.length) {
                        z.execAddress = static_cast<std::uint16_t>(usrN_);
                    }
                    ++t;
                    terminatorAfter(t);
                    blocks_[static_cast<std::size_t>(t)].type = -1;
                    blocks_[static_cast<std::size_t>(t)].startAddress = 0;
                    blocks_[static_cast<std::size_t>(t)].length = 0;
                    blocks_[static_cast<std::size_t>(t)].data = nullptr;
                }
                break;
            }
            case 0x11:  // Turbo data block
                if (!r.has(offset, 19)) {
                    fail("truncated 0x11 block in .tzx file\r\n");
                    throw std::runtime_error("truncated 0x11 block");
                }
                // 3-byte little-endian data length at offset 16.
                offset += 19 + r.le16(offset + 16) + 0x10000u * buffer_[offset + 18];
                warn("non standard block in tzx\r\n");
                break;
            case 0x12:  // Pure tone
                if (!skip(5)) {
                    fail("truncated 0x12 block in .tzx file\r\n");
                    throw std::runtime_error("truncated 0x12 block");
                }
                break;
            case 0x13:  // Pulse sequence
                if (!r.has(offset, 2)) {
                    fail("truncated 0x13 block in .tzx file\r\n");
                    throw std::runtime_error("truncated 0x13 block");
                }
                offset += 2 + 2u * buffer_[offset + 1];
                break;
            case 0x14:  // Pure data block
                if (!r.has(offset, 11)) {
                    fail("truncated 0x14 block in .tzx file\r\n");
                    throw std::runtime_error("truncated 0x14 block");
                }
                // 3-byte little-endian data length at offset 8.
                offset += 11 + r.le16(offset + 8) + 0x10000u * buffer_[offset + 10];
                warn("non standard block in tzx\r\n");
                break;
            case 0x15: {  // Direct recording
                // st_block_15 (original/zxfiles.h) is ID, tstates(2), pause(2),
                // used_bits(1), then a THREE byte little-endian length and the
                // data, so the length sits at +6..8 and the block is 9 + len.
                // The port read it at +3 (which is the pause field) and stepped
                // 6 + len, which desynchronised every later block in the file.
                // There is no variant with an extra field: the T-states are
                // always present.  That is also what k7zx 4.3 did.
                need(9, "truncated 0x15 block in .tzx file\r\n");
                offset += 9 + r.le16(offset + 6) + 0x10000u * buffer_[offset + 8];
                break;
            }
            case 0x18:
            case 0x19:  // Custom info / GS log
                if (!r.has(offset, 5)) {
                    fail("truncated 0x18/0x19 block in .tzx file\r\n");
                    throw std::runtime_error("truncated 0x18/0x19 block");
                }
                offset += 5 + r.le32(offset + 1);
                warn("non standard block in tzx\r\n");
                break;
            case 'Z':  // Minor info
                offset += 10;
                break;
            case 0x20:  // Pause
                offset += 3;
                break;
            case 0x21:  // Group start
                need(2, "truncated 0x21 block in .tzx file\r\n");
                offset += 1 + buffer_[offset + 1] + 1;
                break;
            case 0x22:  // Group end
                offset += 1;
                break;
            case 0x24:  // Loop start
                offset += 3;
                break;
            case 0x25:  // Loop end
                offset += 1;
                break;
            case 0x26:  // Select standard speed
                need(3, "truncated 0x26 block in .tzx file\r\n");
                offset += 3 + 2u * r.le16(offset + 1);
                break;
            case 0x27:  // Custom speed
                offset += 1;
                break;
            case 0x28:  // Pulse
                need(3, "truncated 0x28 block in .tzx file\r\n");
                offset += 3 + r.le16(offset + 1);
                break;
            case 0x2a:  // Set load range: ID + 3 byte offset
                offset += 4;
                break;
            case 0x2b:  // 48K/128K snapshot: ID + 4 bytes
                offset += 5;
                break;
            case 0x30:  // Text description
                need(2, "truncated 0x30 block in .tzx file\r\n");
                offset += 1 + buffer_[offset + 1] + 1;
                break;
            case 0x31:  // 32-bit text description: ID + 4 byte length + text
                need(5, "truncated 0x31 block in .tzx file\r\n");
                offset += 5 + r.le32(offset + 1);
                break;
            case 0x32:  // 16-bit text description
                need(3, "truncated 0x32 block in .tzx file\r\n");
                offset += 1 + buffer_[offset + 1] + 0x100u * buffer_[offset + 2] + 2;
                break;
            case 0x33:  // Tokenized text
                need(2, "truncated 0x33 block in .tzx file\r\n");
                offset += 1 + 3u * buffer_[offset + 1] + 1;
                break;
            case 0x34:  // Inline data
                offset += 9;
                break;
            case 0x35:  // 14 bytes of custom data, then a 4 byte TAP length
                need(19, "truncated 0x35 block in .tzx file\r\n");
                offset += 19 + r.le32(offset + 15);
                break;
            case 0x40:  // Snapshot block: ID + 4 byte length + data
                need(5, "truncated 0x40 block in .tzx file\r\n");
                offset += 5 + r.le32(offset + 1);
                break;
            default:
                // Not a block id.  On the first attempt this is how a 10 byte
                // header masquerades as a 12 byte one; on the last it is a
                // genuine format error.
                fail("error in .tzx file format\r\n");
                throw std::runtime_error("unknown tzx block id");
        }
        if (offset > buffer_.size()) {
            fail("tzx block overruns end of file\r\n");
            throw std::runtime_error("tzx block overruns end of file");
        }
    }
    walkBlocks_ = t;
    return true;
}

// ---------------------------------------------------------------------------
// Snapshot memory fix-ups (preparamemoriasnap)
// ---------------------------------------------------------------------------
void TapeImage::prepareSnapshotMemory(bool m141) {
    std::uint8_t* mem = memory_.data();
    // The original patched the shared `deco_snap` table in place; here the
    // patch lands on this image's private copy.
    std::uint8_t* const deco = deco_.data();

    std::memcpy(mem + 0x4000, pages_[5].data(), 0x4000);
    std::memcpy(mem + 0x8000, pages_[2].data(), 0x4000);
    std::memcpy(mem + 0xc000, pages_[static_cast<std::size_t>(snap_.page & 7)].data(), 0x4000);

    if (snap_.snapshotType == 1) {
        if (m141) {
            blocks_[0].length = static_cast<std::uint16_t>(0xfdbf - 0x4000);
            poke16(deco + 1, 0, 0xfdbf);
            poke16(deco + 4, 0, static_cast<std::uint16_t>(0x4800 - 0x141));
            poke16(deco + 7, 0, 0x241);
            std::memcpy(mem + 0x4800 - 0x141, mem + 0xfdbf, 0x241);
        } else {
            blocks_[0].length = static_cast<std::uint16_t>(0xff00 - 0x4000);
            poke16(deco + 1, 0, 0xff00);
            poke16(deco + 4, 0, 0x4800);
            poke16(deco + 7, 0, 0x100);
            std::memcpy(mem + 0x4800, mem + 0xff00, 0x100);
        }
    } else {
        std::memcpy(mem + 0xc000, pages_[7].data(), 0x4000);
        blocks_[0].length = static_cast<std::uint16_t>(0xbf00 - 0x4000);

        if (snap_.page & 8) {
            std::uint16_t stack128[7];
            stack128[0] = 0x79ed;
            stack128[1] = 0xf1c1;
            stack128[2] = (snap_.imDiBorder & 0x80) ? 0x4ded : 0xc9f3;
            stack128[3] = snap_.BC;
            stack128[4] = snap_.AF;
            stack128[5] = snap_.PC;
            // SP comes straight out of the snapshot header and was never range-checked,
            // so SP < 12 wrote the 12 bytes *before* the RAM array.  With the
            // copy skipped the restore stub still emits `LD SP,SP-6` and
            // `JP SP-12`, so the machine would resume into whatever the file
            // happened to put at $fff8; refusing the snapshot is the only
            // answer that cannot run wild.
            if (snap_.SP < 12) {
                fail("128K .sna snapshot has SP below 12\r\n");
                throw std::runtime_error("128K .sna SP too low");
            }
            std::memcpy(mem + snap_.SP - 12, stack128, 12);

            if (m141) {
                blocks_[0].length = static_cast<std::uint16_t>(0xbdbf - 0x4000);
                poke16(deco + 1, 0, 0xbdbe);
                poke16(deco + 4, 0, static_cast<std::uint16_t>(0xc800 - 0x142));
                poke16(deco + 7, 0, 0x242);
                std::memcpy(mem + 0xc800 - 0x142, mem + 0xbdbe, 0x242);
            } else {
                blocks_[0].length = static_cast<std::uint16_t>(0xbf00 - 0x4000);
                poke16(deco + 1, 0, 0xbf00);
                poke16(deco + 4, 0, 0xc800);
                poke16(deco + 7, 0, 0x100);
                std::memcpy(mem + 0xc800, mem + 0xbf00, 0x100);
            }
        } else {
            std::memcpy(mem + 0x4800, pages_[2].data() + 0x3f00, 0x100);
            if (m141) {
                blocks_[0].length = static_cast<std::uint16_t>(0xbdbf - 0x4000);
                poke16(deco + 1, 0, 0xbdbe);
                poke16(deco + 4, 0, static_cast<std::uint16_t>(0x4800 - 0x142));
                poke16(deco + 7, 0, 0x242);
                std::memcpy(mem + 0x4800 - 0x142, mem + 0xbdbe, 0x242);
            } else {
                blocks_[0].length = static_cast<std::uint16_t>(0xbf00 - 0x4000);
                poke16(deco + 1, 0, 0xbf00);
                poke16(deco + 4, 0, 0x4800);
                poke16(deco + 7, 0, 0x100);
                std::memcpy(mem + 0x4800, mem + 0xbf00, 0x100);
            }
        }
    }

    if (snap_.snapshotType != 1 && (snap_.page & 8)) {
        // Build the 128K restore trampoline at 0xc900.
        std::memcpy(mem + 0xc900, registerBlock(), kRegisterBlockSize);
        std::memcpy(mem + 0xc900 + kRegisterBlockSize, deco, 63);
        int n = 0xc900 + 29 + 45;
        mem[n++] = 0x3e;
        mem[n++] = static_cast<std::uint8_t>(snap_.page);
        mem[n++] = 0x01;
        mem[n++] = 0xfd;
        mem[n++] = 0x7f;
        mem[n++] = 0x31;
        mem[n++] = static_cast<std::uint8_t>((snap_.SP - 6) & 0xff);
        mem[n++] = static_cast<std::uint8_t>((snap_.SP - 6) >> 8);
        mem[n++] = 0xc3;
        mem[n++] = static_cast<std::uint8_t>((snap_.SP - 12) & 0xff);
        mem[n++] = static_cast<std::uint8_t>((snap_.SP - 12) >> 8);
    } else {
        std::memcpy(mem + 0x4900, registerBlock(), kRegisterBlockSize);
        std::memcpy(mem + 0x4900 + kRegisterBlockSize, deco, 63);
    }
}

void TapeImage::prepareSnapshotMemoryForLoaderAt(unsigned base) {
    // The 48K snapshot has to be parked below the restore stub at $4900, so
    // the loader's own RAM has to fit in what is left: [base, $FFFF] has to be
    // no larger than $4900 - $4000.  Every current caller passes $fd40, but the
    // arithmetic silently underflows for a smaller base and the memmove() below
    // then walks a wild pointer across most of the address space.
    if (base < 0x4000 || 0x10000u - base > 0x4900u - 0x4000u) {
        fail("loader base leaves no room to park the 48K snapshot\r\n");
        throw std::runtime_error("loader base too low");
    }
    std::uint8_t* mem = memory_.data();
    std::uint8_t* const deco = deco_.data();
    std::memcpy(mem + 0x4000, pages_[5].data(), 0x4000);
    std::memcpy(mem + 0x8000, pages_[2].data(), 0x4000);
    std::memcpy(mem + 0xc000, pages_[static_cast<std::size_t>(snap_.page & 7)].data(), 0x4000);
    const unsigned len = 0x10000 - base;
    const unsigned park = 0x4900 - len;
    blocks_[0].length = static_cast<std::uint16_t>(base - 0x4000);
    poke16(deco + 1, 0, static_cast<std::uint16_t>(base));  // LD DE,base
    poke16(deco + 4, 0, static_cast<std::uint16_t>(park));  // LD HL,park
    poke16(deco + 7, 0, static_cast<std::uint16_t>(len));   // LD BC,len
    std::memmove(mem + park, mem + base, len);
    std::memcpy(mem + 0x4900, registerBlock(), kRegisterBlockSize);
    std::memcpy(mem + 0x4900 + kRegisterBlockSize, deco, 63);
}

const std::uint8_t* TapeImage::registerBlock() const {
    // The snapshot trampoline expects the saved registers as a tightly packed
    // 29 byte block.  The original got this by memcpy'ing from &snap.HLx in a
    // #pragma pack(1) struct; SnapData is naturally aligned here, so the block
    // is written out field by field instead.  It lives in the object rather
    // than a static so two images cannot alias each other.
    registerBlock_.fill(0);
    std::size_t off = 0;
    auto put = [this, &off](std::uint16_t v) {
        registerBlock_[off++] = static_cast<std::uint8_t>(v & 0xff);
        registerBlock_[off++] = static_cast<std::uint8_t>(v >> 8);
    };
    put(snap_.HLx);
    put(snap_.DEx);
    put(snap_.BCx);
    put(snap_.AFx);
    put(snap_.IY);
    put(snap_.IX);
    put(snap_.HL);
    put(snap_.DE);
    put(snap_.imDiBorder);
    put(snap_.IR);
    put(snap_.BC);
    put(snap_.AF);
    put(snap_.SP);
    registerBlock_[off++] = static_cast<std::uint8_t>(snap_.jp_reti);
    put(snap_.PC);
    return registerBlock_.data();
}

// ---------------------------------------------------------------------------
// .SNA -- 48K and 128K snapshots
// ---------------------------------------------------------------------------
void TapeImage::readSna() {
    constexpr std::size_t kHead = 27;  // sizeof(st_head_sna)
    if (buffer_.size() < kHead) {
        fail("truncated .sna file\r\n");
        throw std::runtime_error("truncated .sna file");
    }
    const ByteReader r(buffer_);

    clearN_ = 0xbdb8;
    usrN_ = 0x4900 + 29;

    BlockInfo& z0 = blocks_[0];
    z0.selected = true;
    z0.type = '3';
    z0.startAddress = 0x4000;
    z0.length = 0xc000;
    z0.data = memory_.data() + 0x4000;
    z0.execAddress = 0x4900 + 29;
    terminatorAfter(1);
    blockCount_ = 1;

    // 48K/64K .SNA header layout:
    //   0 I | 1 HL' | 3 DE' | 5 BC' | 7 AF' | 9 HL | 11 DE | 13 BC
    //   15 AF | 17 SP | 19 IY | 21 IX | 23 IFF2 | 24 R | 25 flags | 26 border
    // (for 64K images, byte 25 carries the port 7FFD value instead).
    // PORTING NOTE: the original's st_head_sna was missing the BC member
    // altogether and mis-placed IY/IX, so these offsets could not have been
    // what the struct described.
    snap_.HLx = r.le16(1);
    snap_.DEx = r.le16(3);
    snap_.BCx = r.le16(5);
    snap_.AFx = r.le16(7);
    snap_.HL = r.le16(9);
    snap_.DE = r.le16(11);
    snap_.BC = r.le16(13);
    snap_.AF = r.le16(15);
    snap_.SP = r.le16(17);
    snap_.IY = r.le16(19);
    snap_.IX = r.le16(21);
    const std::uint8_t iff2 = r.u8(23);
    const std::uint8_t rReg = r.u8(24);
    const std::uint8_t iReg = r.u8(0);
    const std::uint8_t flags = r.u8(25);
    const std::uint8_t border = r.u8(26);
    const int im = flags & 3;
    // Interrupts are masked when IFF2 bit 2 is clear.
    const bool interruptsOn = (iff2 & 0x04) != 0;
    snap_.imDiBorder = static_cast<std::uint16_t>(
        ((interruptsOn ? 0x00 : 0x80) + (border & 7)) * 0x100 + (0x40 * (im == 0 || im == 2)));
    snap_.IR = static_cast<std::uint16_t>((iReg << 8) + rReg);

    if (buffer_.size() == 49179) {
        // 48K: page 5, page 2, page 5 again.
        std::memcpy(pages_[5].data(), buffer_.data() + kHead, 0x4000);
        std::memcpy(pages_[2].data(), buffer_.data() + kHead + 0x4000, 0x4000);
        std::memcpy(pages_[0].data(), buffer_.data() + kHead + 0x8000, 0x4000);
        snap_.jp_reti = 0xed;
        snap_.PC = 0x4545;
        snap_.page = 0;
        snap_.snapshotType = 1;
        return;
    }

    if (buffer_.size() != 131103 && buffer_.size() != 147487) {
        fail("error in .sna file format\r\n");
        throw std::runtime_error("bad .sna size");
    }

    if (buffer_.size() < 27 + 49152 + 3) {
        fail("truncated .sna file\r\n");
        throw std::runtime_error("truncated .sna file");
    }

    std::memcpy(pages_[5].data(), buffer_.data() + kHead, 0x4000);
    std::memcpy(pages_[2].data(), buffer_.data() + kHead + 0x4000, 0x4000);

    snap_.snapshotType = 2;
    snap_.jp_reti = 0xc3;
    snap_.PC = r.le16(27 + 49152);
    snap_.page = static_cast<std::int8_t>(buffer_[27 + 49152 + 2]);

    std::memcpy(pages_[static_cast<std::size_t>(7 & snap_.page)].data(),
                buffer_.data() + kHead + 0x8000, 0x4000);

    multi128_[3 + 20] = static_cast<std::uint8_t>(snap_.page);
    multi128_[3 + 26] = multi128_[3 + 31] = multi128_[3 + 36] = 0x11;
    multi128_[3 + 41] = multi128_[3 + 46] = multi128_[3 + 51] = 0x11;

    if (snap_.page & 8) {
        multi128_[58] = static_cast<std::uint8_t>((snap_.page & 0xf8) | 7);
        multi128_[63] = 0xc9;
    } else {
        multi128_[58] = static_cast<std::uint8_t>(snap_.page);
        multi128_[63] = 0x49;
    }

    std::size_t dirPages = 49183;  // 27 + 49152 + 2 in the original

    int t = 2;
    for (int p = 0; p < 8; ++p) {
        if (p == 2 || p == 5) continue;
        int slot;
        if (p == 7) {
            slot = 1;
        } else if (p == (snap_.page & 0x7)) {
            slot = 6;
        } else {
            slot = t++;
        }
        if (slot >= static_cast<int>(kMaxBlocks) - 1) break;

        BlockInfo& z = blocks_[static_cast<std::size_t>(slot)];
        z.selected = true;
        z.type = '3';
        z.startAddress = 0xc000;
        z.length = 0x4000;

        if (p == (snap_.page & 0x07)) {
            z.data = pages_[static_cast<std::size_t>(p)].data();
        } else {
            if (dirPages + 0x4000 > buffer_.size()) {
                fail("truncated 128K .sna page data\r\n");
                throw std::runtime_error("truncated 128K .sna");
            }
            std::memcpy(pages_[static_cast<std::size_t>(p)].data(), buffer_.data() + dirPages, 0x4000);
            z.data = pages_[static_cast<std::size_t>(p)].data();
            dirPages += 0x4000;
        }
        if (p == 7) z.data = memory_.data() + 0xc000;

        multi128_[3 + 20 + static_cast<std::size_t>(slot) * 5] =
            static_cast<std::uint8_t>((snap_.page & 0xf8) | p);
        multi128_[3 + 21 + static_cast<std::size_t>(slot) * 5] = 0xcd;
    }
    terminatorAfter(7);
    blockCount_ = 7;
}

// ---------------------------------------------------------------------------
// .Z80 -- version 1, 2 and 3 snapshots
// ---------------------------------------------------------------------------
void TapeImage::readZ80() {
    constexpr std::size_t kHead = 30;  // sizeof(st_head_z80)
    if (buffer_.size() < kHead) {
        fail("truncated .z80 file\r\n");
        throw std::runtime_error("truncated .z80 file");
    }
    const ByteReader r(buffer_);

    clearN_ = 0xff0f;
    usrN_ = 0x4900 + 29;

    BlockInfo& z0 = blocks_[0];
    z0.selected = true;
    z0.type = '3';
    z0.startAddress = 0x4000;
    z0.length = 0xc000;
    z0.data = memory_.data() + 0x4000;
    z0.execAddress = 0x4900 + 29;
    terminatorAfter(1);
    blockCount_ = 1;

    // .Z80 header layout:
    //   0-1 AF | 2-3 BC | 4-5 HL | 6-7 PC | 8-9 SP | 10 I | 11 R | 12 flags
    //   13-14 DE | 15-16 BC' | 17-18 DE' | 19-20 HL' | 21-22 AF'
    //   23-24 IY | 25-26 IX | 27 IFF1 | 28 IFF2 | 29 IM/128K flags
    // PORTING NOTE: the original's st_head_z80 laid the alternate registers
    // out in the 64K-SNA order, which does not match the .Z80 format.
    const std::uint8_t flags = r.u8(12);
    const std::uint8_t imFlags = r.u8(29);
    snap_.AF = r.le16(0);
    snap_.BC = r.le16(2);
    snap_.HL = r.le16(4);
    snap_.SP = r.le16(8);
    snap_.DE = r.le16(13);
    snap_.BCx = r.le16(15);
    snap_.DEx = r.le16(17);
    snap_.HLx = r.le16(19);
    snap_.AFx = r.le16(21);
    snap_.IY = r.le16(23);
    snap_.IX = r.le16(25);
    const int im = imFlags & 3;
    const bool interruptsOn = (r.u8(27) & 1) != 0;
    const int border = (flags >> 1) & 7;
    snap_.imDiBorder = static_cast<std::uint16_t>(
        ((interruptsOn ? 0x00 : 0x80) + border) * 0x100 + (0x40 * (im == 0 || im == 2)));
    const std::uint8_t rReg = static_cast<std::uint8_t>((r.u8(11) & 0x7f) | ((flags & 1) ? 0x80 : 0));
    snap_.IR = static_cast<std::uint16_t>((r.u8(10) << 8) + rReg);
    snap_.jp_reti = 0xc3;

    std::uint8_t* mem = memory_.data();

    if (r.le16(6) != 0) {
        // Version 1: a single 48K block of raw or compressed memory.
        snap_.snapshotType = 1;
        snap_.PC = r.le16(6);
        snap_.page = 0;
        if (flags & 32) {  // bit 5: the memory block is compressed
            z80Decompress(mem + 0x4000, 48 * 1024, buffer_.data() + kHead,
                          buffer_.size() - kHead);
        } else {
            if (buffer_.size() < kHead + 48 * 1024) {
                fail("truncated uncompressed 48K .z80 file\r\n");
                throw std::runtime_error("truncated .z80 file");
            }
            std::memcpy(mem + 0x4000, buffer_.data() + kHead, 48 * 1024);
        }
        std::memcpy(pages_[5].data(), mem + 0x4000, 0x4000);
        std::memcpy(pages_[2].data(), mem + 0x8000, 0x4000);
        std::memcpy(pages_[0].data(), mem + 0xc000, 0x4000);
        return;
    }

    // Versions 2 and 3: the 30 byte header, a 16-bit length at 30, the extra
    // header that many bytes long from 32 -- [0..1] PC, [2] hardware, [3] port
    // 7FFD -- then the paged memory blocks.  This is st_head_z80 in k7zx 4.3
    // (long_head2, PC2, hardware_mode, page_ram; procesados = 30 + 2 + long).
    // An earlier transcription read the length as one byte at 30 and the
    // extra header from 31, so every v2/v3 snapshot loaded two bytes out of
    // step: the wrong PC, the wrong paging, and garbage for memory.
    if (!r.has(30, 2)) {
        fail("truncated .z80 file\r\n");
        throw std::runtime_error("truncated .z80 file");
    }
    const std::size_t extraLen = r.le16(30);
    std::size_t processed = kHead + 2 + extraLen;
    if (extraLen < 4 || processed > buffer_.size()) {
        fail("bad .z80 extra header length\r\n");
        throw std::runtime_error("bad .z80 extra header");
    }
    snap_.PC = r.le16(kHead + 2);
    const std::uint8_t hardware = r.u8(kHead + 4);
    const std::uint8_t pageRam = r.u8(kHead + 5);
    snap_.page = static_cast<std::int8_t>(pageRam);
    // Which machine.  k7zx 4.3 took every nonzero hardware byte as a 128K,
    // which also sent 48K + Interface 1 snapshots down the 128K path.  The
    // format: version 2 (extra length 23) 3, 4 = 128K; version 3 4.. = 128K.
    const bool is128 = extraLen == 23 ? hardware >= 3 : hardware >= 4;

    // A page block: [length][page][data].  Version 3 marks a page stored
    // uncompressed with length 0xFFFF (then exactly 16384 bytes follow); k7zx
    // 4.3 ran those through the RLE decoder and stepped 65535 bytes on.
    auto stored = [](std::uint16_t len) -> std::size_t { return len == 0xffff ? kPageSize : len; };
    auto unpack = [&](std::uint8_t* dst, std::size_t at, std::uint16_t len) {
        const std::size_t from = at + 3;
        const std::size_t avail = from < buffer_.size() ? buffer_.size() - from : 0;
        const std::size_t n = std::min(stored(len), avail);
        if (len == 0xffff)
            std::memcpy(dst, buffer_.data() + from, n);
        else
            z80Decompress(dst, kPageSize, buffer_.data() + from, n);
    };

    if (!is128) {
        snap_.snapshotType = 1;
        while (processed < buffer_.size()) {
            if (!r.has(processed, 3)) break;
            const std::uint16_t pageLen = r.le16(processed);
            switch (r.u8(processed + 2)) {
                case 8: unpack(pages_[5].data(), processed, pageLen); break;
                case 4: unpack(pages_[2].data(), processed, pageLen); break;
                case 5:
                    unpack(pages_[static_cast<std::size_t>(7 & snap_.page)].data(), processed, pageLen);
                    break;
                default:
                    break;
            }
            processed += 3 + stored(pageLen);
        }
        return;
    }

    snap_.snapshotType = 2;
    // The 128K design has exactly seven page slots (0..6) and a seven-entry
    // block list; multi128_ lays its page-swap hooks out at 3 + 20 + slot * 5.
    // `slot` below used to be bounded only by kMaxBlocks, so a file that
    // repeats one bank -- which the format permits -- walked it far past the
    // hook table and wrote over the BASIC stub that follows it in memory.
    static_assert(3 + 21 + (kMulti128Slots - 1) * 5 < kMulti128Size,
                  "the page-swap hook table must hold one 5-byte entry per slot");

    multi128_[3 + 20] = pageRam;
    multi128_[3 + 26] = multi128_[3 + 31] = multi128_[3 + 36] = 0x11;
    multi128_[3 + 41] = multi128_[3 + 46] = multi128_[3 + 51] = 0x11;

    if (snap_.page & 8) {
        multi128_[58] = static_cast<std::uint8_t>((snap_.page & 0xf8) | 7);
        multi128_[63] = 0xc9;
    } else {
        multi128_[58] = static_cast<std::uint8_t>(snap_.page);
        multi128_[63] = 0x49;
    }

    int t = 2;
    bool warnedSlotOverflow = false;
    while (processed < buffer_.size()) {
        if (!r.has(processed, 3)) break;
        const std::uint16_t pageLen = r.le16(processed);
        const std::uint8_t pageNum = r.u8(processed + 2);

        if (pageNum == 8) {
            unpack(pages_[5].data(), processed, pageLen);
        } else if (pageNum == 5) {
            unpack(pages_[2].data(), processed, pageLen);
        } else if (pageNum >= 3) {
            const int bank = pageNum - 3;
            if (bank > 7) {
                processed += 3 + stored(pageLen);
                continue;
            }
            int slot;
            if (bank == 7) {
                slot = 1;
            } else if (bank == (pageRam & 7)) {
                slot = 6;
            } else {
                slot = t++;
            }
            // Bound the slot by what the page-swap hook table and the seven
            // entry block list can actually describe, not by kMaxBlocks.
            // A file may repeat a bank; the surplus pages are then skipped.
            if (slot >= kMulti128Slots) {
                if (!warnedSlotOverflow) {
                    warn("more than 7 memory pages\r\n");
                    warnedSlotOverflow = true;
                }
                processed += 3 + stored(pageLen);
                continue;
            }

            BlockInfo& z = blocks_[static_cast<std::size_t>(slot)];
            z.type = '3';
            z.startAddress = 0xc000;
            z.length = 0x4000;
            if (pageLen != 260) {
                unpack(pages_[static_cast<std::size_t>(bank)].data(), processed, pageLen);
                z.data = (slot == 1) ? (mem + 0xc000) : pages_[static_cast<std::size_t>(bank)].data();
                multi128_[3 + 20 + static_cast<std::size_t>(slot) * 5] =
                    static_cast<std::uint8_t>((pageRam & 0xf8) | bank);
                multi128_[3 + 21 + static_cast<std::size_t>(slot) * 5] = 0xcd;
                z.selected = true;
            } else {
                // Unused (uncompressed) page: nothing to send.
                z.selected = false;
                z.data = pages_[static_cast<std::size_t>(bank)].data();
            }
        }
        processed += 3 + stored(pageLen);
    }
    // After the loop, not before: the page loop assigns blocks_[slot].type for
    // slots 0..6, and the block list documents entry [blockCount] as always
    // being the terminator.
    terminatorAfter(kMulti128Slots);
    blockCount_ = kMulti128Slots;
}

// ---------------------------------------------------------------------------
// .SBB -- SCL86 tape-block container (ZX machine only)
// ---------------------------------------------------------------------------
void TapeImage::readSbb() {
    // original/zxfiles.h, under #pragma pack(1):
    //   ts_sbb_header  sbb_version[3] machine[5] extra_info[8] nombre[16]
    //                  caux origin n_blocks poke_ffff clear_sp usr_pc  = 40
    //   ts_sbb_block   blockname[16] size param3 block_type h_chksum
    //                  ini jump exec d_chksum data[0x10000]           = 28
    // The port used 48 for the header, read clear_sp and usr_pc a byte early and
    // read `ini` at 23 instead of 22, so every field after the name was shifted
    // and real .sbb files could not be parsed.
    constexpr std::size_t kHead = 40;  // sizeof(ts_sbb_header)
    constexpr std::size_t kBlockHead = 28;  // sizeof(ts_sbb_block) minus data[]
    if (buffer_.size() < kHead) {
        warn("not sb file\r\n");
        return;
    }
    const ByteReader r(buffer_);
    if (buffer_[0] != 'S' || buffer_[1] != 'B') {
        warn("not sb file\r\n");
        return;
    }
    if (buffer_[3] != 'Z') {
        warn("non zx data\r\n");
        return;
    }

    snap_.snapshotType = 0;
    clearN_ = r.le16(36);  // clear_sp
    usrN_ = r.le16(38);    // usr_pc

    std::size_t offset = kHead;
    int t = 0;
    while (offset < buffer_.size()) {
        if (offset + kBlockHead > buffer_.size()) {
            warn("truncated block in .sbb file\r\n");
            break;
        }
        if (t >= static_cast<int>(kMaxBlocks) - 1) break;
        const std::uint16_t size = r.le16(offset + 16);
        if (offset + kBlockHead + size > buffer_.size()) {
            warn("block overruns .sbb file\r\n");
            break;
        }

        BlockInfo& z = blocks_[static_cast<std::size_t>(t)];
        z.selected = true;
        z.type = '3';
        z.length = size;
        z.startAddress = r.le16(offset + 22);  // ini
        z.execAddress = 0;                     // dir_exe, as in the original
        z.data = buffer_.data() + offset + kBlockHead;

        if (!usrN_) usrN_ = r.le16(offset + 25);  // exec, not jump

        const char blockType = static_cast<char>(buffer_[offset + 20]);
        if (blockType == '6') {
            snapshotBlocks_ = true;
        } else if (blockType == '7') {
            snapshotBlocks_ = true;
        } else if (static_cast<unsigned>(z.startAddress) + z.length > kMaxAddress) {
            warn("block overrides address $ff3c\r\n");
        }
        ++t;
        offset += kBlockHead + size;
    }
    terminatorAfter(t);
    blockCount_ = t;
    if (blockCount_ > 7) warn("more than 7 blocks\r\n");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
ReadError TapeImage::read(const std::string& path) {
    reset();
    if (path.empty()) {
        fail("no file name\r\n");
        return ReadError::open;
    }

    try {
        buffer_ = readFile(path);
    } catch (const std::bad_alloc&) {
        fail("out of memory reading the file\r\n");
        return ReadError::memory;
    } catch (const std::exception&) {
        fail("cannot open file\r\n");
        return ReadError::open;
    }

    if (buffer_.empty()) {
        fail("error reading file\r\n");
        return ReadError::io;
    }

    ReadError result = ReadError::none;
    try {
        switch (formatFromPath(path)) {
            case TapeFormat::hex: readHex(); break;
            case TapeFormat::tap: readTap(); break;
            case TapeFormat::tzx: readTzx(); break;
            case TapeFormat::sna: readSna(); break;
            case TapeFormat::z80: readZ80(); break;
            case TapeFormat::sbb: readSbb(); break;
            case TapeFormat::unknown:
                fail("unsupported file extension\r\n");
                return ReadError::format;
        }
    } catch (const std::bad_alloc&) {
        if (errors_.empty()) fail("out of memory reading the file\r\n");
        result = ReadError::memory;
    } catch (const std::exception& e) {
        // ByteReader's message carries the offset, the length and the size, so
        // it is the only useful thing to show for a truncated file.
        if (errors_.empty()) fail(std::string("error parsing file: ") + e.what() + "\r\n");
        result = ReadError::format;
    }

    if (prgName_.empty()) prgName_ = trimProgramName(baseName(path));
    if (clearN_ > kMaxAddress) warn("CLEAR overrides $ff3c\r\n");
    return result;
}

}  // namespace k7zx
