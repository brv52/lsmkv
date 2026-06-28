// Internal key format shared by memtables and SSTables.
//
//   internal_key := user_key | fixed64(tag)    tag := (sequence << 8) | ValueType
//
// Internal keys sort by user key ascending, then by tag descending, so the
// newest version of a key is encountered first.
#pragma once

#include <cstdint>
#include <string>

#include "lsm/db.hpp"
#include "lsm/slice.hpp"
#include "util/coding.hpp"

namespace lsm {

using SequenceNumber = uint64_t;
inline constexpr SequenceNumber kMaxSequenceNumber = (uint64_t{1} << 56) - 1;
// Type used when seeking: kValue (1) sorts before kDeletion (0) at equal seq.
inline constexpr ValueType kValueTypeForSeek = ValueType::kValue;

constexpr uint64_t PackTag(SequenceNumber seq, ValueType t) noexcept {
  return (seq << 8) | static_cast<uint64_t>(t);
}

inline Slice ExtractUserKey(const Slice& ikey) noexcept { return {ikey.data(), ikey.size() - 8}; }
inline uint64_t ExtractTag(const Slice& ikey) noexcept { return DecodeFixed64(ikey.data() + ikey.size() - 8); }
inline SequenceNumber ExtractSequence(const Slice& ikey) noexcept { return ExtractTag(ikey) >> 8; }
inline ValueType ExtractValueType(const Slice& ikey) noexcept {
  return static_cast<ValueType>(ExtractTag(ikey) & 0xff);
}

inline void AppendInternalKey(std::string* dst, const Slice& user_key, SequenceNumber seq, ValueType t) {
  dst->append(user_key.data(), user_key.size());
  PutFixed64(dst, PackTag(seq, t));
}
inline std::string MakeInternalKey(const Slice& user_key, SequenceNumber seq, ValueType t) {
  std::string r;
  r.reserve(user_key.size() + 8);
  AppendInternalKey(&r, user_key, seq, t);
  return r;
}

struct InternalKeyComparator {
  int operator()(const Slice& a, const Slice& b) const noexcept { return Compare(a, b); }
  static int Compare(const Slice& a, const Slice& b) noexcept {
    int r = ExtractUserKey(a).compare(ExtractUserKey(b));
    if (r == 0) {
      const uint64_t ta = ExtractTag(a), tb = ExtractTag(b);
      if (ta > tb) r = -1;
      else if (ta < tb) r = 1;
    }
    return r;
  }
};

// Key used to probe a memtable: varint32(ikey_len) | user_key | tag.
class LookupKey {
 public:
  LookupKey(const Slice& user_key, SequenceNumber seq) {
    const size_t ilen = user_key.size() + 8;
    PutVarint32(&buf_, static_cast<uint32_t>(ilen));
    kstart_ = buf_.size();
    AppendInternalKey(&buf_, user_key, seq, kValueTypeForSeek);
  }
  [[nodiscard]] Slice memtable_key() const noexcept { return {buf_.data(), buf_.size()}; }
  [[nodiscard]] Slice internal_key() const noexcept { return {buf_.data() + kstart_, buf_.size() - kstart_}; }
  [[nodiscard]] Slice user_key() const noexcept { return {buf_.data() + kstart_, buf_.size() - kstart_ - 8}; }

 private:
  std::string buf_;
  size_t kstart_ = 0;
};

}  // namespace lsm
