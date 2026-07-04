// Cache-local (blocked) bloom filter.
//
// Each key hashes (MurmurHash3) to one 64-byte cache line and sets k bits
// inside it, so a probe costs a single cache miss. At 10 bits/key and k = 6
// the false-positive rate is ~1% (slightly above a classic bloom filter's
// 0.8%, in exchange for 1 miss instead of k).
//
//   layout := lines[num_lines * 64] | uint8 k | fixed32 num_lines
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "lsm/slice.hpp"
#include "util/coding.hpp"
#include "util/hash.hpp"

namespace lsm::sst {

inline uint64_t BloomHash(const Slice& user_key) noexcept { return Hash64(user_key.data(), user_key.size(), 0xb100f); }

class BloomFilterBuilder {
 public:
  explicit BloomFilterBuilder(int bits_per_key) : bits_per_key_(bits_per_key < 1 ? 1 : bits_per_key) {}

  void AddHash(uint64_t h) {
    if (hashes_.empty() || hashes_.back() != h) hashes_.push_back(h);
  }
  [[nodiscard]] size_t num_keys() const noexcept { return hashes_.size(); }

  [[nodiscard]] std::string Finish() const {
    const uint64_t bits = std::max<uint64_t>(512, hashes_.size() * static_cast<uint64_t>(bits_per_key_));
    const auto num_lines = static_cast<uint32_t>((bits + 511) / 512);
    // ln(2) * bits/key, minus one: a slightly lower k is optimal for blocked filters.
    int k = static_cast<int>(bits_per_key_ * 0.69 + 0.5) - 1;
    k = k < 1 ? 1 : (k > 24 ? 24 : k);
    std::string out(static_cast<size_t>(num_lines) * 64, '\0');
    for (uint64_t h : hashes_) Set(out.data(), num_lines, k, h);
    out.push_back(static_cast<char>(k));
    PutFixed32(&out, num_lines);
    return out;
  }

  static bool MayContain(const Slice& filter, uint64_t h) noexcept {
    if (filter.size() < 5) return true;  // Missing / malformed filter: never a false negative.
    const int k = static_cast<unsigned char>(filter[filter.size() - 5]);
    const uint32_t num_lines = DecodeFixed32(filter.data() + filter.size() - 4);
    if (num_lines == 0 || static_cast<uint64_t>(num_lines) * 64 + 5 != filter.size()) return true;
    const auto* line = reinterpret_cast<const uint8_t*>(filter.data()) + LineIndex(h, num_lines) * 64;
    auto h2 = static_cast<uint32_t>(h);
    for (int i = 0; i < k; ++i) {
      const uint32_t bit = h2 >> (32 - 9);  // Top 9 bits: position within the 512-bit line.
      if ((line[bit >> 3] & (1u << (bit & 7))) == 0) return false;
      h2 *= 0x9e3779b9u;
    }
    return true;
  }

 private:
  static uint64_t LineIndex(uint64_t h, uint32_t num_lines) noexcept {
    // Lemire's fast range reduction on the upper 32 bits.
    return (static_cast<uint64_t>(h >> 32) * num_lines) >> 32;
  }

  static void Set(char* data, uint32_t num_lines, int k, uint64_t h) noexcept {
    auto* line = reinterpret_cast<uint8_t*>(data) + LineIndex(h, num_lines) * 64;
    auto h2 = static_cast<uint32_t>(h);
    for (int i = 0; i < k; ++i) {
      const uint32_t bit = h2 >> (32 - 9);
      line[bit >> 3] |= static_cast<uint8_t>(1u << (bit & 7));
      h2 *= 0x9e3779b9u;
    }
  }

  const int bits_per_key_;
  std::vector<uint64_t> hashes_;
};

}  // namespace lsm::sst
