// Sharded LRU block cache.
//
// O_DIRECT bypasses the kernel page cache, so this cache is the only thing
// standing between a point lookup and the device. Entries are keyed by
// (sstable file number, block offset) and hold shared_ptrs: a reader pins a
// block simply by holding its pointer, and eviction only drops the cache's
// reference. File numbers are never reused, so entries of deleted tables are
// unreachable and age out.
#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace lsm {

struct CacheKey {
  uint64_t file_number;
  uint64_t offset;
  friend bool operator==(const CacheKey&, const CacheKey&) = default;
};

class LruCache {
 public:
  // capacity in bytes, split evenly across 2^shard_bits shards.
  LruCache(size_t capacity, int shard_bits);
  LruCache(const LruCache&) = delete;
  LruCache& operator=(const LruCache&) = delete;

  [[nodiscard]] std::shared_ptr<const void> Lookup(const CacheKey& key);
  // Replaces an existing entry with the same key.
  void Insert(const CacheKey& key, std::shared_ptr<const void> value, size_t charge);
  void Erase(const CacheKey& key);

  [[nodiscard]] size_t TotalCharge() const;
  [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] uint64_t hits() const noexcept { return hits_.load(std::memory_order_relaxed); }
  [[nodiscard]] uint64_t misses() const noexcept { return misses_.load(std::memory_order_relaxed); }

 private:
  struct KeyHash {
    size_t operator()(const CacheKey& k) const noexcept;
  };
  struct Entry {
    CacheKey key;
    std::shared_ptr<const void> value;
    size_t charge;
  };
  struct alignas(64) Shard {
    mutable std::mutex mu;
    std::list<Entry> lru;  // Front = most recently used.
    std::unordered_map<CacheKey, std::list<Entry>::iterator, KeyHash> map;
    size_t usage = 0;
    size_t capacity = 0;
  };

  Shard& ShardFor(const CacheKey& key) noexcept;
  static void EvictLocked(Shard& s);

  const size_t capacity_;
  const int shard_bits_;
  std::vector<Shard> shards_;
  std::atomic<uint64_t> hits_{0};
  std::atomic<uint64_t> misses_{0};
};

}  // namespace lsm
