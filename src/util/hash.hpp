// MurmurHash3 x64_128 (Austin Appleby, public domain), used for bloom filters
// and cache sharding.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace lsm {

namespace detail {
constexpr uint64_t Rotl64(uint64_t x, int r) noexcept { return (x << r) | (x >> (64 - r)); }
constexpr uint64_t Fmix64(uint64_t k) noexcept {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33;
  return k;
}
}  // namespace detail

inline std::pair<uint64_t, uint64_t> Murmur3_128(const void* key, size_t len, uint64_t seed = 0) noexcept {
  using detail::Rotl64;
  const auto* data = static_cast<const unsigned char*>(key);
  const size_t nblocks = len / 16;
  uint64_t h1 = seed, h2 = seed;
  constexpr uint64_t c1 = 0x87c37b91114253d5ULL;
  constexpr uint64_t c2 = 0x4cf5ad432745937fULL;

  for (size_t i = 0; i < nblocks; ++i) {
    uint64_t k1, k2;
    std::memcpy(&k1, data + i * 16, 8);
    std::memcpy(&k2, data + i * 16 + 8, 8);
    k1 *= c1; k1 = Rotl64(k1, 31); k1 *= c2; h1 ^= k1;
    h1 = Rotl64(h1, 27); h1 += h2; h1 = h1 * 5 + 0x52dce729;
    k2 *= c2; k2 = Rotl64(k2, 33); k2 *= c1; h2 ^= k2;
    h2 = Rotl64(h2, 31); h2 += h1; h2 = h2 * 5 + 0x38495ab5;
  }

  const unsigned char* tail = data + nblocks * 16;
  uint64_t k1 = 0, k2 = 0;
  switch (len & 15) {
    case 15: k2 ^= uint64_t(tail[14]) << 48; [[fallthrough]];
    case 14: k2 ^= uint64_t(tail[13]) << 40; [[fallthrough]];
    case 13: k2 ^= uint64_t(tail[12]) << 32; [[fallthrough]];
    case 12: k2 ^= uint64_t(tail[11]) << 24; [[fallthrough]];
    case 11: k2 ^= uint64_t(tail[10]) << 16; [[fallthrough]];
    case 10: k2 ^= uint64_t(tail[9]) << 8; [[fallthrough]];
    case 9:
      k2 ^= uint64_t(tail[8]);
      k2 *= c2; k2 = Rotl64(k2, 33); k2 *= c1; h2 ^= k2;
      [[fallthrough]];
    case 8: k1 ^= uint64_t(tail[7]) << 56; [[fallthrough]];
    case 7: k1 ^= uint64_t(tail[6]) << 48; [[fallthrough]];
    case 6: k1 ^= uint64_t(tail[5]) << 40; [[fallthrough]];
    case 5: k1 ^= uint64_t(tail[4]) << 32; [[fallthrough]];
    case 4: k1 ^= uint64_t(tail[3]) << 24; [[fallthrough]];
    case 3: k1 ^= uint64_t(tail[2]) << 16; [[fallthrough]];
    case 2: k1 ^= uint64_t(tail[1]) << 8; [[fallthrough]];
    case 1:
      k1 ^= uint64_t(tail[0]);
      k1 *= c1; k1 = Rotl64(k1, 31); k1 *= c2; h1 ^= k1;
      break;
    default: break;
  }

  h1 ^= len; h2 ^= len;
  h1 += h2; h2 += h1;
  h1 = detail::Fmix64(h1);
  h2 = detail::Fmix64(h2);
  h1 += h2; h2 += h1;
  return {h1, h2};
}

inline uint64_t Hash64(const void* key, size_t len, uint64_t seed = 0) noexcept {
  return Murmur3_128(key, len, seed).first;
}

}  // namespace lsm
