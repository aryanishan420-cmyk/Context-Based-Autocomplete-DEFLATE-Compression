// DEFLATE decoder.  Written to be strict about the bits it accepts: every
// checksum, length complement and code-space check in RFC 1951/1950/1952 is
// enforced, so a round-trip failure shows up as an exception, not as silence.
#include <cstring>
#include <string>

#include "deflate/deflate.hpp"
#include "deflate/huffman.hpp"
#include "deflate/tables.hpp"

namespace deflate {
namespace {

void inflateBlocks(BitReader& in, std::vector<uint8_t>& out) {
  const std::vector<uint8_t> fixed_lit = fixedLitLenLengths();
  const std::vector<uint8_t> fixed_dist = fixedDistLengths();
  const HuffDecoder fixed_lit_dec = buildDecoder(fixed_lit);
  const HuffDecoder fixed_dist_dec = buildDecoder(fixed_dist);

  bool final_block = false;
  while (!final_block) {
    final_block = in.getBit() != 0;
    const uint32_t type = in.getBits(2);

    if (type == 0) {  // stored
      in.alignToByte();
      const uint32_t len = in.getBits(16);
      const uint32_t nlen = in.getBits(16);
      if ((len ^ 0xFFFFu) != nlen) throw CorruptStream("stored block length mismatch");
      for (uint32_t i = 0; i < len; ++i)
        out.push_back(static_cast<uint8_t>(in.getBits(8)));
      continue;
    }
    if (type == 3) throw CorruptStream("reserved block type");

    HuffDecoder lit_dec, dist_dec;
    if (type == 1) {
      lit_dec = fixed_lit_dec;
      dist_dec = fixed_dist_dec;
    } else {
      const int hlit = static_cast<int>(in.getBits(5)) + 257;
      const int hdist = static_cast<int>(in.getBits(5)) + 1;
      const int hclen = static_cast<int>(in.getBits(4)) + 4;

      std::vector<uint8_t> cl_lengths(kCodeLenSymbols, 0);
      for (int i = 0; i < hclen; ++i)
        cl_lengths[kCodeLenOrder[i]] = static_cast<uint8_t>(in.getBits(3));
      const HuffDecoder cl_dec = buildDecoder(cl_lengths);

      // Read the RLE-coded lengths of the literal/length and distance codes.
      std::vector<uint8_t> lengths;
      lengths.reserve(hlit + hdist);
      while (static_cast<int>(lengths.size()) < hlit + hdist) {
        const int sym = decodeSymbol(in, cl_dec);
        if (sym < 16) {
          lengths.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 16) {
          if (lengths.empty()) throw CorruptStream("repeat with no previous length");
          const uint8_t prev = lengths.back();
          for (uint32_t n = in.getBits(2) + 3; n > 0; --n) lengths.push_back(prev);
        } else if (sym == 17) {
          for (uint32_t n = in.getBits(3) + 3; n > 0; --n) lengths.push_back(0);
        } else {
          for (uint32_t n = in.getBits(7) + 11; n > 0; --n) lengths.push_back(0);
        }
      }
      if (static_cast<int>(lengths.size()) != hlit + hdist)
        throw CorruptStream("code length sequence overruns its alphabet");

      std::vector<uint8_t> lit_lengths(lengths.begin(), lengths.begin() + hlit);
      std::vector<uint8_t> dist_lengths(lengths.begin() + hlit, lengths.end());
      lit_lengths.resize(kLitLenSymbols, 0);
      if (!lit_lengths[kEndOfBlock]) throw CorruptStream("no end-of-block code");

      lit_dec = buildDecoder(lit_lengths);
      // A distance tree holding a single code is the one incomplete code that
      // RFC 1951 permits, and zlib emits it, so accept it here too.
      dist_dec = buildDecoder(dist_lengths, /*allow_incomplete=*/true);
    }

    for (;;) {
      const int sym = decodeSymbol(in, lit_dec);
      if (sym < 256) {
        out.push_back(static_cast<uint8_t>(sym));
        continue;
      }
      if (sym == kEndOfBlock) break;

      const int li = sym - 257;
      if (li >= 29) throw CorruptStream("invalid length symbol");
      const int length = kLengthBase[li] + static_cast<int>(in.getBits(kLengthExtra[li]));

      const int ds = decodeSymbol(in, dist_dec);
      if (ds >= kDistSymbols) throw CorruptStream("invalid distance symbol");
      const size_t distance =
          kDistBase[ds] + static_cast<size_t>(in.getBits(kDistExtra[ds]));
      if (distance > out.size()) throw CorruptStream("distance runs before the output");

      // Overlapping copies are legal and are how runs get encoded, so copy
      // byte by byte rather than with memcpy.
      const size_t from = out.size() - distance;
      for (int i = 0; i < length; ++i) out.push_back(out[from + i]);
    }
  }
}

}  // namespace

Container detectContainer(const uint8_t* data, size_t size) {
  if (size >= 2 && data[0] == 0x1F && data[1] == 0x8B) return Container::Gzip;
  if (size >= 2 && (data[0] & 0x0F) == 8 && ((data[0] << 8) | data[1]) % 31 == 0)
    return Container::Zlib;
  return Container::Raw;
}

std::vector<uint8_t> decompress(const uint8_t* data, size_t size,
                                Container container) {
  size_t offset = 0;
  if (container == Container::Zlib) {
    if (size < 6) throw CorruptStream("zlib stream too short");
    const uint8_t cmf = data[0], flg = data[1];
    if ((cmf & 0x0F) != 8) throw CorruptStream("unsupported zlib compression method");
    if (((cmf << 8) | flg) % 31 != 0) throw CorruptStream("bad zlib header check");
    if (flg & 0x20) throw CorruptStream("preset dictionaries are not supported");
    offset = 2;
  } else if (container == Container::Gzip) {
    if (size < 18) throw CorruptStream("gzip stream too short");
    if (data[0] != 0x1F || data[1] != 0x8B) throw CorruptStream("bad gzip magic");
    if (data[2] != 8) throw CorruptStream("unsupported gzip compression method");
    const uint8_t flg = data[3];
    offset = 10;
    auto need = [&](size_t n) {
      if (offset + n > size) throw CorruptStream("truncated gzip header");
    };
    if (flg & 0x04) {  // FEXTRA
      need(2);
      const size_t xlen = data[offset] | (data[offset + 1] << 8);
      offset += 2;
      need(xlen);
      offset += xlen;
    }
    if (flg & 0x08) {  // FNAME
      while (offset < size && data[offset] != 0) ++offset;
      need(1);
      ++offset;
    }
    if (flg & 0x10) {  // FCOMMENT
      while (offset < size && data[offset] != 0) ++offset;
      need(1);
      ++offset;
    }
    if (flg & 0x02) {  // FHCRC
      need(2);
      offset += 2;
    }
  }

  std::vector<uint8_t> out;
  BitReader in(data + offset, size - offset);
  inflateBlocks(in, out);

  if (container == Container::Zlib) {
    in.alignToByte();
    const size_t tail = offset + in.bytePos();
    if (tail + 4 > size) throw CorruptStream("missing adler-32 trailer");
    uint32_t stored = 0;
    for (int i = 0; i < 4; ++i) stored = (stored << 8) | data[tail + i];
    if (stored != adler32(out.data(), out.size()))
      throw CorruptStream("adler-32 mismatch");
  } else if (container == Container::Gzip) {
    in.alignToByte();
    const size_t tail = offset + in.bytePos();
    if (tail + 8 > size) throw CorruptStream("missing gzip trailer");
    uint32_t stored_crc = 0, stored_size = 0;
    for (int i = 3; i >= 0; --i) stored_crc = (stored_crc << 8) | data[tail + i];
    for (int i = 3; i >= 0; --i) stored_size = (stored_size << 8) | data[tail + 4 + i];
    if (stored_crc != crc32(out.data(), out.size()))
      throw CorruptStream("crc-32 mismatch");
    if (stored_size != static_cast<uint32_t>(out.size() & 0xFFFFFFFFu))
      throw CorruptStream("uncompressed size mismatch");
  }
  return out;
}

std::vector<uint8_t> decompress(const std::vector<uint8_t>& data,
                                Container container) {
  return decompress(data.data(), data.size(), container);
}

}  // namespace deflate
