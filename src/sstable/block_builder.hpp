// Builds prefix-compressed blocks.
//
//   entry   := varint32 shared | varint32 non_shared | varint32 value_len |
//              key[shared..] (non_shared bytes) | value
//   trailer := fixed32 restart[0..n) | fixed32 n
//
// Every `restart_interval` entries the key is stored in full and its offset
// recorded as a restart point, which Seek() binary-searches.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lsm/slice.hpp"

namespace lsm::sst {

class BlockBuilder {
 public:
  explicit BlockBuilder(int restart_interval);

  // REQUIRES: key > every key added since the last Reset().
  void Add(const Slice& key, const Slice& value);
  // Appends the restart array; the result stays valid until Reset().
  Slice Finish();
  void Reset();

  [[nodiscard]] size_t CurrentSizeEstimate() const noexcept {
    return buffer_.size() + restarts_.size() * 4 + 4;
  }
  [[nodiscard]] bool empty() const noexcept { return buffer_.empty(); }
  [[nodiscard]] const std::string& last_key() const noexcept { return last_key_; }

 private:
  const int restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;
  bool finished_ = false;
  std::string last_key_;
};

}  // namespace lsm::sst
