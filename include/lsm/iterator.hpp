// Ordered cursor interface shared by memtables, blocks, tables and the DB.
#pragma once

#include "lsm/slice.hpp"
#include "lsm/status.hpp"

namespace lsm {

class Iterator {
 public:
  Iterator() = default;
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;
  virtual ~Iterator() = default;

  [[nodiscard]] virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  // Position at the first entry with key >= target.
  virtual void Seek(const Slice& target) = 0;
  virtual void Next() = 0;
  // REQUIRES: Valid(). The returned slices stay valid until the iterator moves.
  [[nodiscard]] virtual Slice key() const = 0;
  [[nodiscard]] virtual Slice value() const = 0;
  [[nodiscard]] virtual Status status() const = 0;
};

}  // namespace lsm
