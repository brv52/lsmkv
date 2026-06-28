// CRC32C (Castagnoli). Uses SSE4.2 crc32 instructions when the CPU has them
// (checked once at startup), otherwise a slicing-by-8 table implementation.
#pragma once

#include <cstddef>
#include <cstdint>

namespace lsm::crc32c {

// Returns crc32c(concat(A, data[0,n))) where init_crc = crc32c(A).
uint32_t Extend(uint32_t init_crc, const char* data, size_t n) noexcept;

inline uint32_t Value(const char* data, size_t n) noexcept { return Extend(0, data, n); }

// True when the hardware path is active.
bool HardwareAccelerated() noexcept;

// Stored CRCs are masked so that computing the CRC of a string that itself
// contains embedded CRCs does not degrade error detection, and so that an
// all-zero region never carries a valid checksum.
inline constexpr uint32_t kMaskDelta = 0xa282ead8u;
inline uint32_t Mask(uint32_t crc) noexcept { return ((crc >> 15) | (crc << 17)) + kMaskDelta; }
inline uint32_t Unmask(uint32_t masked) noexcept {
  const uint32_t rot = masked - kMaskDelta;
  return (rot >> 17) | (rot << 15);
}

}  // namespace lsm::crc32c
