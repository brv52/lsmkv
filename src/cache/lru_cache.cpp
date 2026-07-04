#include "cache/lru_cache.hpp"

#include "util/hash.hpp"

namespace lsm {

size_t LruCache::KeyHash::operator()(const CacheKey& k) const noexcept {
  return static_cast<size_t>(detail::Fmix64(k.file_number * 0x9e3779b97f4a7c15ULL ^ k.offset));
}

LruCache::LruCache(size_t capacity, int shard_bits)
    : capacity_(capacity),
      shard_bits_(shard_bits < 0 ? 0 : (shard_bits > 10 ? 10 : shard_bits)),
      shards_(size_t{1} << shard_bits_) {
  const size_t per_shard = (capacity + shards_.size() - 1) / shards_.size();
  for (auto& s : shards_) s.capacity = per_shard;
}

LruCache::Shard& LruCache::ShardFor(const CacheKey& key) noexcept {
  const uint64_t h = KeyHash{}(key);
  return shards_[shard_bits_ == 0 ? 0 : (h >> (64 - shard_bits_))];
}

std::shared_ptr<const void> LruCache::Lookup(const CacheKey& key) {
  Shard& s = ShardFor(key);
  std::lock_guard lk(s.mu);
  auto it = s.map.find(key);
  if (it == s.map.end()) {
    misses_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  s.lru.splice(s.lru.begin(), s.lru, it->second);
  hits_.fetch_add(1, std::memory_order_relaxed);
  return it->second->value;
}

void LruCache::EvictLocked(Shard& s) {
  while (s.usage > s.capacity && !s.lru.empty()) {
    Entry& victim = s.lru.back();
    s.usage -= victim.charge;
    s.map.erase(victim.key);
    s.lru.pop_back();  // Readers still holding the shared_ptr keep the block alive.
  }
}

void LruCache::Insert(const CacheKey& key, std::shared_ptr<const void> value, size_t charge) {
  if (capacity_ == 0) return;
  Shard& s = ShardFor(key);
  std::shared_ptr<const void> displaced;  // Released outside the lock.
  std::lock_guard lk(s.mu);
  if (auto it = s.map.find(key); it != s.map.end()) {
    s.usage -= it->second->charge;
    displaced = std::move(it->second->value);
    it->second->value = std::move(value);
    it->second->charge = charge;
    s.usage += charge;
    s.lru.splice(s.lru.begin(), s.lru, it->second);
  } else {
    s.lru.push_front(Entry{key, std::move(value), charge});
    s.map.emplace(key, s.lru.begin());
    s.usage += charge;
  }
  EvictLocked(s);
}

void LruCache::Erase(const CacheKey& key) {
  Shard& s = ShardFor(key);
  std::lock_guard lk(s.mu);
  auto it = s.map.find(key);
  if (it == s.map.end()) return;
  s.usage -= it->second->charge;
  s.lru.erase(it->second);
  s.map.erase(it);
}

size_t LruCache::TotalCharge() const {
  size_t total = 0;
  for (const auto& s : shards_) {
    std::lock_guard lk(s.mu);
    total += s.usage;
  }
  return total;
}

}  // namespace lsm
