// k7zx 5.0 - modern C++ port
//
// Explicit little-endian accessors.  The original code relied on the packed
// structs declared with `#pragma pack(push,1)` being laid out little-endian on
// x86; spelling the conversions out keeps the port correct on any host and
// makes every multi-byte read bounds-checked by the caller.
#ifndef K7ZX_BYTEORDER_H
#define K7ZX_BYTEORDER_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace k7zx {

/// A read-only view over the tape image, with bounds-checked little-endian
/// readers.  Reads past the end throw instead of walking off the heap.
class ByteReader {
public:
    ByteReader() = default;
    ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    explicit ByteReader(const std::vector<std::uint8_t>& v) : data_(v.data()), size_(v.size()) {}

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    const std::uint8_t* data() const { return data_; }

    /// True when [off, off+len) lies inside the buffer.
    bool has(std::size_t off, std::size_t len = 1) const { return off <= size_ && len <= size_ - off; }

    void require(std::size_t off, std::size_t len = 1) const {
        if (!has(off, len)) {
            throw std::out_of_range("k7zx: read past end of tape image (offset " +
                                    std::to_string(off) + ", length " + std::to_string(len) +
                                    ", size " + std::to_string(size_) + ")");
        }
    }

    std::uint8_t u8(std::size_t off) const {
        require(off);
        return data_[off];
    }

    std::uint16_t le16(std::size_t off) const {
        require(off, 2);
        return static_cast<std::uint16_t>(data_[off]) |
               (static_cast<std::uint16_t>(data_[off + 1]) << 8);
    }

    std::uint32_t le32(std::size_t off) const {
        require(off, 4);
        return static_cast<std::uint32_t>(data_[off]) |
               (static_cast<std::uint32_t>(data_[off + 1]) << 8) |
               (static_cast<std::uint32_t>(data_[off + 2]) << 16) |
               (static_cast<std::uint32_t>(data_[off + 3]) << 24);
    }

    /// Pointer to the raw bytes at [off, off+len).
    const std::uint8_t* ptr(std::size_t off, std::size_t len) const {
        require(off, len);
        return data_ + off;
    }

    std::string text(std::size_t off, std::size_t len) const {
        require(off, len);
        return std::string(reinterpret_cast<const char*>(data_ + off), len);
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

/// Little-endian sequential writer, used to build the RIFF/WAVE header.
class ByteWriter {
public:
    void u8(std::uint8_t v) { buf_.push_back(v); }
    void le16(std::uint16_t v) {
        buf_.push_back(static_cast<std::uint8_t>(v & 0xff));
        buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    }
    void le32(std::uint32_t v) {
        buf_.push_back(static_cast<std::uint8_t>(v & 0xff));
        buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
        buf_.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
        buf_.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
    }
    void tag(const char* fourcc) { for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<std::uint8_t>(fourcc[i])); }
    void raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    std::size_t size() const { return buf_.size(); }
    std::vector<std::uint8_t>& buffer() { return buf_; }

private:
    std::vector<std::uint8_t> buf_;
};

/// Overwrite 16 bits at `off` inside an existing little-endian buffer.
inline void poke16(std::uint8_t* base, std::size_t off, std::uint16_t v) {
    base[off] = static_cast<std::uint8_t>(v & 0xff);
    base[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
}

/// 16-bit little-endian load from an arbitrary buffer.
inline std::uint16_t peek16(const std::uint8_t* base, std::size_t off) {
    return static_cast<std::uint16_t>(base[off]) | (static_cast<std::uint16_t>(base[off + 1]) << 8);
}

/// Read a whole file.  Throws std::runtime_error on failure.
std::vector<std::uint8_t> readFile(const std::string& path);

/// Write `bytes` to `path`, replacing any existing file.
void writeFile(const std::string& path, const std::uint8_t* bytes, std::size_t size);

}  // namespace k7zx

#endif  // K7ZX_BYTEORDER_H
