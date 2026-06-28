// Little-endian fixed-width and varint encoding helpers.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "lsm/slice.hpp"

namespace lsm {

inline void EncodeFixed16(char* dst, uint16_t v) noexcept { std::memcpy(dst, &v, sizeof(v)); }
inline void EncodeFixed32(char* dst, uint32_t v) noexcept { std::memcpy(dst, &v, sizeof(v)); }
inline void EncodeFixed64(char* dst, uint64_t v) noexcept { std::memcpy(dst, &v, sizeof(v)); }

inline uint16_t DecodeFixed16(const char* p) noexcept {
  uint16_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline uint32_t DecodeFixed32(const char* p) noexcept {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
inline uint64_t DecodeFixed64(const char* p) noexcept {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "on-disk format assumes little-endian hosts");

inline void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  EncodeFixed32(buf, v);
  dst->append(buf, 4);
}
inline void PutFixed64(std::string* dst, uint64_t v) {
  char buf[8];
  EncodeFixed64(buf, v);
  dst->append(buf, 8);
}

inline char* EncodeVarint32(char* dst, uint32_t v) noexcept {
  auto* p = reinterpret_cast<unsigned char*>(dst);
  while (v >= 0x80) {
    *p++ = static_cast<unsigned char>(v | 0x80);
    v >>= 7;
  }
  *p++ = static_cast<unsigned char>(v);
  return reinterpret_cast<char*>(p);
}
inline char* EncodeVarint64(char* dst, uint64_t v) noexcept {
  auto* p = reinterpret_cast<unsigned char*>(dst);
  while (v >= 0x80) {
    *p++ = static_cast<unsigned char>(v | 0x80);
    v >>= 7;
  }
  *p++ = static_cast<unsigned char>(v);
  return reinterpret_cast<char*>(p);
}

inline void PutVarint32(std::string* dst, uint32_t v) {
  char buf[5];
  dst->append(buf, static_cast<size_t>(EncodeVarint32(buf, v) - buf));
}
inline void PutVarint64(std::string* dst, uint64_t v) {
  char buf[10];
  dst->append(buf, static_cast<size_t>(EncodeVarint64(buf, v) - buf));
}
inline void PutLengthPrefixedSlice(std::string* dst, const Slice& s) {
  PutVarint32(dst, static_cast<uint32_t>(s.size()));
  dst->append(s.data(), s.size());
}

constexpr int VarintLength(uint64_t v) noexcept {
  int len = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++len;
  }
  return len;
}

// Returns pointer past the varint, or nullptr if malformed / truncated.
inline const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value) noexcept {
  uint32_t result = 0;
  for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
    const uint32_t byte = static_cast<unsigned char>(*p++);
    if (byte & 0x80) {
      result |= (byte & 0x7f) << shift;
    } else {
      result |= byte << shift;
      *value = result;
      return p;
    }
  }
  return nullptr;
}
inline const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) noexcept {
  uint64_t result = 0;
  for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
    const uint64_t byte = static_cast<unsigned char>(*p++);
    if (byte & 0x80) {
      result |= (byte & 0x7f) << shift;
    } else {
      result |= byte << shift;
      *value = result;
      return p;
    }
  }
  return nullptr;
}

inline bool GetVarint32(Slice* input, uint32_t* value) noexcept {
  const char* p = GetVarint32Ptr(input->data(), input->data() + input->size(), value);
  if (p == nullptr) return false;
  input->remove_prefix(static_cast<size_t>(p - input->data()));
  return true;
}
inline bool GetVarint64(Slice* input, uint64_t* value) noexcept {
  const char* p = GetVarint64Ptr(input->data(), input->data() + input->size(), value);
  if (p == nullptr) return false;
  input->remove_prefix(static_cast<size_t>(p - input->data()));
  return true;
}
inline bool GetFixed64(Slice* input, uint64_t* value) noexcept {
  if (input->size() < 8) return false;
  *value = DecodeFixed64(input->data());
  input->remove_prefix(8);
  return true;
}
inline bool GetLengthPrefixedSlice(Slice* input, Slice* result) noexcept {
  uint32_t len;
  if (!GetVarint32(input, &len) || input->size() < len) return false;
  *result = Slice(input->data(), len);
  input->remove_prefix(len);
  return true;
}

}  // namespace lsm
