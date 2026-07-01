// In-memory write buffer: a concurrent skiplist of encoded entries.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "lsm/iterator.hpp"
#include "memtable/arena.hpp"
#include "memtable/skiplist.hpp"
#include "util/dbformat.hpp"

namespace lsm {

class MemTable {
 public:
  MemTable() : table_(KeyComparator{}, &arena_) {}
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Thread-safe; may run concurrently with other Add() and readers.
  void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value);

  // Returns true if the newest entry for key with sequence <= the lookup
  // sequence was found: *s is OK with *value filled for a put, NotFound for
  // a deletion.
  bool Get(const LookupKey& key, std::string* value, Status* s) const;

  // Iterates internal keys in InternalKeyComparator order. The memtable must
  // outlive the iterator.
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator() const;

  [[nodiscard]] size_t ApproximateMemoryUsage() const noexcept { return arena_.MemoryUsage(); }
  [[nodiscard]] uint64_t num_entries() const noexcept { return entries_.load(std::memory_order_relaxed); }
  [[nodiscard]] bool empty() const noexcept { return num_entries() == 0; }

  // WAL that holds this memtable's contents; set when it becomes active.
  uint64_t log_number = 0;
  // Highest sequence number allocated to this memtable. Flushing must wait
  // until the group-commit queue has published it.
  uint64_t last_sequence = 0;

 private:
  struct KeyComparator {
    // Entries are varint32-length-prefixed internal keys.
    int operator()(const char* a, const char* b) const noexcept;
  };
  using Table = SkipList<const char*, KeyComparator>;
  friend class MemTableIterator;

  ConcurrentArena arena_;
  Table table_;
  std::atomic<uint64_t> entries_{0};
};

// Decodes a varint32-length-prefixed slice at p.
inline Slice GetLengthPrefixed(const char* p) noexcept {
  uint32_t len = 0;
  const char* q = GetVarint32Ptr(p, p + 5, &len);
  return {q, len};
}

}  // namespace lsm
