// The Huffman half of DEFLATE: turns the LZ77 token stream into blocks, and
// picks stored / fixed / dynamic per block by computing each one's exact bit
// cost rather than guessing.
#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "deflate/deflate.hpp"
#include "deflate/huffman.hpp"
#include "deflate/tables.hpp"

namespace deflate {
namespace {

// One entry of the RLE-coded code-length sequence of a dynamic block.
struct CodeLenItem {
  uint8_t symbol;      // 0-15 literal length, 16/17/18 repeat codes
  uint8_t extra_bits;
  uint16_t extra_value;
};

std::vector<CodeLenItem> rleCodeLengths(const std::vector<uint8_t>& seq) {
  std::vector<CodeLenItem> out;
  size_t i = 0;
  while (i < seq.size()) {
    const uint8_t value = seq[i];
    size_t run = 1;
    while (i + run < seq.size() && seq[i + run] == value) ++run;

    if (value == 0) {
      while (run >= 11) {
        uint16_t take = static_cast<uint16_t>(std::min<size_t>(run, 138));
        out.push_back({18, 7, static_cast<uint16_t>(take - 11)});
        run -= take;
        i += take;
      }
      while (run >= 3) {
        uint16_t take = static_cast<uint16_t>(std::min<size_t>(run, 10));
        out.push_back({17, 3, static_cast<uint16_t>(take - 3)});
        run -= take;
        i += take;
      }
      while (run-- > 0) {
        out.push_back({0, 0, 0});
        ++i;
      }
    } else {
      out.push_back({value, 0, 0});
      ++i;
      --run;
      // Symbol 16 repeats the *previous* length 3-6 times.
      while (run >= 3) {
        uint16_t take = static_cast<uint16_t>(std::min<size_t>(run, 6));
        out.push_back({16, 2, static_cast<uint16_t>(take - 3)});
        run -= take;
        i += take;
      }
      while (run-- > 0) {
        out.push_back({value, 0, 0});
        ++i;
      }
    }
  }
  return out;
}

// Everything needed to both price and emit one dynamic block header.
struct DynamicHeader {
  std::vector<uint8_t> lit_lengths;
  std::vector<uint8_t> dist_lengths;
  std::vector<uint8_t> cl_lengths;
  std::vector<CodeLenItem> items;
  int hlit = 257, hdist = 1, hclen = 4;
  uint64_t header_bits = 0;
};

// Guarantees a complete (Kraft sum == 1) code by handing out a second one-bit
// code when the alphabet would otherwise hold a single symbol.  Some decoders
// reject the incomplete form, and the extra code costs a couple of bits.
void ensureTwoCodes(std::vector<uint8_t>& lengths) {
  int used = 0;
  for (uint8_t l : lengths)
    if (l) ++used;
  if (used >= 2) return;
  for (size_t i = 0; i < lengths.size() && used < 2; ++i) {
    if (!lengths[i]) {
      lengths[i] = 1;
      ++used;
    }
  }
  for (size_t i = 0; i < lengths.size(); ++i)
    if (lengths[i]) lengths[i] = 1;
}

DynamicHeader buildDynamicHeader(const std::vector<uint64_t>& lit_freq,
                                 const std::vector<uint64_t>& dist_freq) {
  DynamicHeader h;
  h.lit_lengths = packageMerge(lit_freq, kMaxCodeLen);
  h.dist_lengths = packageMerge(dist_freq, kMaxCodeLen);
  ensureTwoCodes(h.lit_lengths);
  ensureTwoCodes(h.dist_lengths);

  h.hlit = 257;
  for (int i = 285; i >= 257; --i)
    if (h.lit_lengths[i]) {
      h.hlit = i + 1;
      break;
    }
  h.hdist = 1;
  for (int i = kDistSymbols - 1; i >= 1; --i)
    if (h.dist_lengths[i]) {
      h.hdist = i + 1;
      break;
    }

  std::vector<uint8_t> seq;
  seq.reserve(h.hlit + h.hdist);
  seq.insert(seq.end(), h.lit_lengths.begin(), h.lit_lengths.begin() + h.hlit);
  seq.insert(seq.end(), h.dist_lengths.begin(), h.dist_lengths.begin() + h.hdist);
  h.items = rleCodeLengths(seq);

  std::vector<uint64_t> cl_freq(kCodeLenSymbols, 0);
  for (const CodeLenItem& it : h.items) cl_freq[it.symbol]++;
  h.cl_lengths = packageMerge(cl_freq, 7);

  h.hclen = 4;
  for (int i = kCodeLenSymbols - 1; i >= 4; --i)
    if (h.cl_lengths[kCodeLenOrder[i]]) {
      h.hclen = i + 1;
      break;
    }

  h.header_bits = 3 + 5 + 5 + 4 + 3ull * h.hclen;
  for (const CodeLenItem& it : h.items)
    h.header_bits += h.cl_lengths[it.symbol] + it.extra_bits;
  return h;
}

// Bits needed for the token payload (including the end-of-block symbol).
uint64_t payloadBits(const std::vector<uint64_t>& lit_freq,
                     const std::vector<uint64_t>& dist_freq,
                     const std::vector<uint8_t>& lit_lengths,
                     const std::vector<uint8_t>& dist_lengths,
                     uint64_t extra_bits) {
  uint64_t bits = extra_bits;
  for (size_t s = 0; s < lit_freq.size(); ++s) {
    if (!lit_freq[s]) continue;
    if (!lit_lengths[s]) return UINT64_MAX;  // symbol has no code (fixed tree)
    bits += lit_freq[s] * lit_lengths[s];
  }
  for (size_t s = 0; s < dist_freq.size(); ++s) {
    if (!dist_freq[s]) continue;
    if (!dist_lengths[s]) return UINT64_MAX;
    bits += dist_freq[s] * dist_lengths[s];
  }
  return bits;
}

void emitTokens(BitWriter& bw, const Token* tokens, size_t count,
                const HuffEncoder& lit, const HuffEncoder& dist) {
  for (size_t i = 0; i < count; ++i) {
    const Token& t = tokens[i];
    if (!t.isMatch()) {
      bw.putCode(lit.code[t.lit], lit.length[t.lit]);
      continue;
    }
    const CodedValue len = encodeLength(t.len);
    bw.putCode(lit.code[len.symbol], lit.length[len.symbol]);
    if (len.extra_bits) bw.putBits(len.extra_value, len.extra_bits);

    const CodedValue d = encodeDistance(t.dist);
    bw.putCode(dist.code[d.symbol], dist.length[d.symbol]);
    if (d.extra_bits) bw.putBits(d.extra_value, d.extra_bits);
  }
  bw.putCode(lit.code[kEndOfBlock], lit.length[kEndOfBlock]);
}

}  // namespace

std::vector<uint8_t> compress(const uint8_t* data, size_t size,
                              const Options& opts, Report* report) {
  std::vector<uint8_t> out;
  Report local;
  Report& rep = report ? *report : local;
  rep = Report();
  rep.input_bytes = size;

  if (opts.container == Container::Zlib) {
    // CMF: deflate, 32 KiB window.  FLG carries the level hint and a check
    // value that makes the 16-bit header a multiple of 31.
    uint8_t cmf = 0x78;
    uint8_t flevel = opts.level <= 1 ? 0 : (opts.level <= 5 ? 1 : (opts.level <= 6 ? 2 : 3));
    uint8_t flg = static_cast<uint8_t>(flevel << 6);
    flg |= static_cast<uint8_t>(31 - ((cmf * 256 + flg) % 31));
    out.push_back(cmf);
    out.push_back(flg);
  } else if (opts.container == Container::Gzip) {
    const bool named = !opts.filename.empty();
    out.insert(out.end(), {0x1F, 0x8B, 0x08, static_cast<uint8_t>(named ? 0x08 : 0x00),
                           0, 0, 0, 0,
                           static_cast<uint8_t>(opts.level == 9 ? 2 : (opts.level <= 1 ? 4 : 0)),
                           0xFF});
    if (named) {
      out.insert(out.end(), opts.filename.begin(), opts.filename.end());
      out.push_back(0);
    }
  }

  const Lz77Params params = Lz77Params::forLevel(opts.level);
  const std::vector<Token> tokens = lz77Parse(data, size, params, &rep.lz77);

  BitWriter bw(out);
  const std::vector<uint8_t> fixed_lit = fixedLitLenLengths();
  const std::vector<uint8_t> fixed_dist = fixedDistLengths();
  const HuffEncoder fixed_lit_enc = buildEncoder(fixed_lit);
  const HuffEncoder fixed_dist_enc = buildEncoder(fixed_dist);

  size_t tok = 0, byte_pos = 0;
  bool wrote_block = false;

  while (tok < tokens.size() || !wrote_block) {
    // Collect one block worth of tokens and note how many input bytes it covers.
    const size_t block_start_tok = tok;
    const size_t block_start_byte = byte_pos;
    std::vector<uint64_t> lit_freq(kLitLenSymbols, 0);
    std::vector<uint64_t> dist_freq(kDistSymbols, 0);
    uint64_t extra_bits = 0;
    size_t block_bytes = 0;

    while (tok < tokens.size() && tok - block_start_tok < opts.block_tokens) {
      const Token& t = tokens[tok];
      if (!t.isMatch()) {
        lit_freq[t.lit]++;
        block_bytes += 1;
      } else {
        const CodedValue len = encodeLength(t.len);
        const CodedValue d = encodeDistance(t.dist);
        lit_freq[len.symbol]++;
        dist_freq[d.symbol]++;
        extra_bits += len.extra_bits + d.extra_bits;
        block_bytes += t.len;
      }
      ++tok;
    }
    lit_freq[kEndOfBlock]++;
    byte_pos += block_bytes;

    const bool final_block = (tok >= tokens.size());

    // Price the three block types.
    const DynamicHeader dyn = buildDynamicHeader(lit_freq, dist_freq);
    const uint64_t dyn_payload = payloadBits(lit_freq, dist_freq, dyn.lit_lengths,
                                             dyn.dist_lengths, extra_bits);
    const uint64_t dyn_bits = dyn.header_bits + dyn_payload;

    const uint64_t fixed_payload =
        payloadBits(lit_freq, dist_freq, fixed_lit, fixed_dist, extra_bits);
    const uint64_t fixed_bits =
        (fixed_payload == UINT64_MAX) ? UINT64_MAX : 3 + fixed_payload;

    uint64_t stored_bits = UINT64_MAX;
    if (block_bytes <= 65535) {
      const uint64_t pad = (8 - ((bw.bitsWritten() + 3) % 8)) % 8;
      stored_bits = 3 + pad + 32 + 8ull * block_bytes;
    }

    const uint64_t before = bw.bitsWritten();
    const char* chosen;

    if (stored_bits <= dyn_bits && stored_bits <= fixed_bits) {
      chosen = "stored";
      bw.putBits(final_block ? 1 : 0, 1);
      bw.putBits(0, 2);
      bw.alignToByte();
      const uint16_t len = static_cast<uint16_t>(block_bytes);
      bw.putBits(len, 16);
      bw.putBits(static_cast<uint16_t>(~len), 16);
      for (size_t i = 0; i < block_bytes; ++i)
        bw.putBits(data[block_start_byte + i], 8);
    } else if (fixed_bits <= dyn_bits) {
      chosen = "fixed";
      bw.putBits(final_block ? 1 : 0, 1);
      bw.putBits(1, 2);
      emitTokens(bw, tokens.data() + block_start_tok, tok - block_start_tok,
                 fixed_lit_enc, fixed_dist_enc);
    } else {
      chosen = "dynamic";
      bw.putBits(final_block ? 1 : 0, 1);
      bw.putBits(2, 2);
      bw.putBits(dyn.hlit - 257, 5);
      bw.putBits(dyn.hdist - 1, 5);
      bw.putBits(dyn.hclen - 4, 4);
      for (int i = 0; i < dyn.hclen; ++i)
        bw.putBits(dyn.cl_lengths[kCodeLenOrder[i]], 3);

      const HuffEncoder cl_enc = buildEncoder(dyn.cl_lengths);
      for (const CodeLenItem& it : dyn.items) {
        bw.putCode(cl_enc.code[it.symbol], cl_enc.length[it.symbol]);
        if (it.extra_bits) bw.putBits(it.extra_value, it.extra_bits);
      }
      emitTokens(bw, tokens.data() + block_start_tok, tok - block_start_tok,
                 buildEncoder(dyn.lit_lengths), buildEncoder(dyn.dist_lengths));
    }

    rep.blocks.push_back({chosen, tok - block_start_tok, block_bytes,
                          bw.bitsWritten() - before});
    wrote_block = true;
  }

  bw.flush();

  if (opts.container == Container::Zlib) {
    const uint32_t sum = adler32(data, size);
    for (int shift = 24; shift >= 0; shift -= 8)  // big endian
      out.push_back(static_cast<uint8_t>((sum >> shift) & 0xFF));
  } else if (opts.container == Container::Gzip) {
    const uint32_t sum = crc32(data, size);
    const uint32_t isize = static_cast<uint32_t>(size & 0xFFFFFFFFu);
    for (int shift = 0; shift <= 24; shift += 8)  // little endian
      out.push_back(static_cast<uint8_t>((sum >> shift) & 0xFF));
    for (int shift = 0; shift <= 24; shift += 8)
      out.push_back(static_cast<uint8_t>((isize >> shift) & 0xFF));
  }

  rep.output_bytes = out.size();
  return out;
}

std::vector<uint8_t> compress(const std::vector<uint8_t>& data,
                              const Options& opts, Report* report) {
  return compress(data.data(), data.size(), opts, report);
}

}  // namespace deflate
