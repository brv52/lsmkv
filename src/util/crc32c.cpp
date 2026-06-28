#include "util/crc32c.hpp"

#include <array>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#define LSM_CRC32C_X86 1
#endif

namespace lsm::crc32c {
namespace {

constexpr uint32_t kPoly = 0x82f63b78u;  // Reflected Castagnoli polynomial.

using Table = std::array<std::array<uint32_t, 256>, 8>;

constexpr Table MakeTable() {
  Table t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ kPoly : c >> 1;
    t[0][i] = c;
  }
  for (uint32_t i = 0; i < 256; ++i) {
    for (size_t s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xff];
  }
  return t;
}

constexpr Table kTable = MakeTable();

uint32_t ExtendSoftware(uint32_t crc, const char* data, size_t n) noexcept {
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  uint32_t c = crc ^ 0xffffffffu;
  while (n > 0 && (reinterpret_cast<uintptr_t>(p) & 7) != 0) {
    c = kTable[0][(c ^ *p++) & 0xff] ^ (c >> 8);
    --n;
  }
  while (n >= 8) {
    uint64_t w;
    std::memcpy(&w, p, 8);
    w ^= c;
    c = kTable[7][w & 0xff] ^ kTable[6][(w >> 8) & 0xff] ^ kTable[5][(w >> 16) & 0xff] ^
        kTable[4][(w >> 24) & 0xff] ^ kTable[3][(w >> 32) & 0xff] ^ kTable[2][(w >> 40) & 0xff] ^
        kTable[1][(w >> 48) & 0xff] ^ kTable[0][w >> 56];
    p += 8;
    n -= 8;
  }
  while (n-- > 0) c = kTable[0][(c ^ *p++) & 0xff] ^ (c >> 8);
  return c ^ 0xffffffffu;
}

#ifdef LSM_CRC32C_X86
__attribute__((target("sse4.2"))) uint32_t ExtendHardware(uint32_t crc, const char* data,
                                                          size_t n) noexcept {
  const auto* p = reinterpret_cast<const unsigned char*>(data);
  uint32_t c32 = crc ^ 0xffffffffu;
  while (n > 0 && (reinterpret_cast<uintptr_t>(p) & 7) != 0) {
    c32 = __builtin_ia32_crc32qi(c32, *p++);
    --n;
  }
#ifdef __x86_64__
  uint64_t c64 = c32;
  // Unrolled 32 bytes / iteration keeps the crc32 unit busy.
  while (n >= 32) {
    uint64_t w0, w1, w2, w3;
    std::memcpy(&w0, p, 8);
    std::memcpy(&w1, p + 8, 8);
    std::memcpy(&w2, p + 16, 8);
    std::memcpy(&w3, p + 24, 8);
    c64 = __builtin_ia32_crc32di(c64, w0);
    c64 = __builtin_ia32_crc32di(c64, w1);
    c64 = __builtin_ia32_crc32di(c64, w2);
    c64 = __builtin_ia32_crc32di(c64, w3);
    p += 32;
    n -= 32;
  }
  while (n >= 8) {
    uint64_t w;
    std::memcpy(&w, p, 8);
    c64 = __builtin_ia32_crc32di(c64, w);
    p += 8;
    n -= 8;
  }
  c32 = static_cast<uint32_t>(c64);
#endif
  while (n >= 4) {
    uint32_t w;
    std::memcpy(&w, p, 4);
    c32 = __builtin_ia32_crc32si(c32, w);
    p += 4;
    n -= 4;
  }
  while (n-- > 0) c32 = __builtin_ia32_crc32qi(c32, *p++);
  return c32 ^ 0xffffffffu;
}
#endif

using ExtendFn = uint32_t (*)(uint32_t, const char*, size_t) noexcept;

ExtendFn Choose() noexcept {
#ifdef LSM_CRC32C_X86
  __builtin_cpu_init();
  if (__builtin_cpu_supports("sse4.2")) return &ExtendHardware;
#endif
  return &ExtendSoftware;
}

const ExtendFn kExtend = Choose();

}  // namespace

uint32_t Extend(uint32_t init_crc, const char* data, size_t n) noexcept { return kExtend(init_crc, data, n); }

bool HardwareAccelerated() noexcept { return kExtend != &ExtendSoftware; }

}  // namespace lsm::crc32c
