// The fixed alphabets and extra-bit tables of RFC 1951, section 3.2.5-3.2.6.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace deflate {

constexpr int kLitLenSymbols = 288;  // 0-255 literals, 256 EOB, 257-285 lengths
constexpr int kDistSymbols = 30;
constexpr int kCodeLenSymbols = 19;
constexpr int kEndOfBlock = 256;

// Length symbols 257..285.
constexpr std::array<uint16_t, 29> kLengthBase = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<uint8_t, 29> kLengthExtra = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

constexpr std::array<uint16_t, 30> kDistBase = {
    1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<uint8_t, 30> kDistExtra = {
    0, 0, 0, 0, 1, 1, 2,  2,  3,  3,  4,  4,  5,  5,  6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// The code-length alphabet is itself Huffman coded, with its lengths written
// in this deliberately front-loaded order so trailing zeros can be dropped.
constexpr std::array<uint8_t, kCodeLenSymbols> kCodeLenOrder = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct CodedValue {
  uint16_t symbol;
  uint8_t extra_bits;
  uint16_t extra_value;
};

inline CodedValue encodeLength(int len) {
  int i = 28;
  while (i > 0 && kLengthBase[i] > len) --i;
  return {static_cast<uint16_t>(257 + i), kLengthExtra[i],
          static_cast<uint16_t>(len - kLengthBase[i])};
}

inline CodedValue encodeDistance(int dist) {
  int i = 29;
  while (i > 0 && kDistBase[i] > dist) --i;
  return {static_cast<uint16_t>(i), kDistExtra[i],
          static_cast<uint16_t>(dist - kDistBase[i])};
}

// Fixed Huffman code lengths (BTYPE = 01).
inline std::vector<uint8_t> fixedLitLenLengths() {
  std::vector<uint8_t> l(kLitLenSymbols);
  for (int i = 0; i < 144; ++i) l[i] = 8;
  for (int i = 144; i < 256; ++i) l[i] = 9;
  for (int i = 256; i < 280; ++i) l[i] = 7;
  for (int i = 280; i < 288; ++i) l[i] = 8;
  return l;
}

inline std::vector<uint8_t> fixedDistLengths() {
  return std::vector<uint8_t>(32, 5);
}

}  // namespace deflate
