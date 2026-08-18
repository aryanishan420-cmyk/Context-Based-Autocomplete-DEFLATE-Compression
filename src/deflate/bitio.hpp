// Bit-level I/O for the DEFLATE stream (RFC 1951, section 3.1.1).
//
// DEFLATE packs bits into bytes starting from the least-significant bit, but
// Huffman codes are written starting from the *most* significant bit of the
// code.  Both conventions are handled here so the rest of the code never has
// to think about bit order again.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace deflate {

class CorruptStream : public std::runtime_error {
 public:
  explicit CorruptStream(const std::string& what)
      : std::runtime_error("corrupt deflate stream: " + what) {}
};

// Reverses the low `len` bits of `code`; used to turn a canonical Huffman code
// (MSB first) into something the LSB-first bit writer can emit directly.
inline uint32_t reverseBits(uint32_t code, int len) {
  uint32_t out = 0;
  for (int i = 0; i < len; ++i) {
    out = (out << 1) | (code & 1u);
    code >>= 1;
  }
  return out;
}

class BitWriter {
 public:
  explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}

  // Writes `count` bits of `value`, least-significant bit first.
  void putBits(uint32_t value, int count) {
    buf_ |= static_cast<uint64_t>(value & ((count == 32) ? 0xFFFFFFFFu
                                                        : ((1u << count) - 1)))
            << nbits_;
    nbits_ += count;
    total_ += count;
    while (nbits_ >= 8) {
      out_.push_back(static_cast<uint8_t>(buf_ & 0xFF));
      buf_ >>= 8;
      nbits_ -= 8;
    }
  }

  // Writes a canonical Huffman code, most-significant bit of the code first.
  void putCode(uint32_t code, int len) { putBits(reverseBits(code, len), len); }

  // Pads the current byte with zero bits (used before a stored block).
  void alignToByte() {
    if (nbits_ > 0) putBits(0, 8 - nbits_);
  }

  void flush() { alignToByte(); }
  size_t bitsWritten() const { return total_; }

 private:
  std::vector<uint8_t>& out_;
  uint64_t buf_ = 0;
  int nbits_ = 0;
  size_t total_ = 0;
};

class BitReader {
 public:
  BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  // Reads `count` bits, least-significant bit first.
  uint32_t getBits(int count) {
    while (nbits_ < count) {
      if (pos_ >= size_) throw CorruptStream("unexpected end of input");
      buf_ |= static_cast<uint64_t>(data_[pos_++]) << nbits_;
      nbits_ += 8;
    }
    uint32_t value = static_cast<uint32_t>(buf_ & ((1ull << count) - 1));
    buf_ >>= count;
    nbits_ -= count;
    return value;
  }

  int getBit() { return static_cast<int>(getBits(1)); }

  void alignToByte() {
    int drop = nbits_ % 8;
    if (drop) getBits(drop);
  }

  // Number of whole bytes consumed so far (only valid on a byte boundary).
  size_t bytePos() const { return pos_ - static_cast<size_t>(nbits_ / 8); }

  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
  uint64_t buf_ = 0;
  int nbits_ = 0;
};

}  // namespace deflate
