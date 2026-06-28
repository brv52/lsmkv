// Public database API.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lsm/iterator.hpp"
#include "lsm/options.hpp"
#include "lsm/slice.hpp"
#include "lsm/status.hpp"

namespace lsm {

enum class ValueType : uint8_t {
  kDeletion = 0,
  kValue = 1,
};

// Ordered list of mutations applied atomically with respect to readers and
// assigned consecutive sequence numbers.
class WriteBatch {
 public:
  struct Op {
    ValueType type;
    std::string key;
    std::string value;
  };

  void Put(const Slice& key, const Slice& value) {
    ops_.push_back({ValueType::kValue, key.ToString(), value.ToString()});
    bytes_ += key.size() + value.size();
  }
  void Delete(const Slice& key) {
    ops_.push_back({ValueType::kDeletion, key.ToString(), {}});
    bytes_ += key.size();
  }
  void Clear() {
    ops_.clear();
    bytes_ = 0;
  }

  [[nodiscard]] size_t Count() const noexcept { return ops_.size(); }
  [[nodiscard]] size_t ApproximateBytes() const noexcept { return bytes_; }
  [[nodiscard]] const std::vector<Op>& ops() const noexcept { return ops_; }

 private:
  std::vector<Op> ops_;
  size_t bytes_ = 0;
};

struct DBStats {
  uint64_t user_bytes_written = 0;    // Keys + values accepted from callers.
  uint64_t wal_bytes_written = 0;     // Bytes submitted to WAL files (incl. padding/rewrites).
  uint64_t flush_bytes_written = 0;   // SSTable bytes produced by memtable flushes.
  uint64_t compaction_bytes_written = 0;
  uint64_t compaction_bytes_read = 0;
  uint64_t group_commits = 0;
  uint64_t group_commit_writers = 0;
  uint64_t block_cache_hits = 0;
  uint64_t block_cache_misses = 0;
  uint64_t bloom_useful = 0;          // Lookups short-circuited by a bloom filter.
  uint64_t io_bytes_written = 0;      // Everything the IoBackend wrote (WAL + tables).
  uint64_t io_bytes_read = 0;
  uint64_t write_stalls = 0;
  std::vector<int> files_per_level;
  std::vector<uint64_t> bytes_per_level;
  std::string io_backend;
};

class DB {
 public:
  static Status Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db);
  // Remove every file belonging to the database at `path`.
  static Status Destroy(const std::string& path);

  DB() = default;
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;
  virtual ~DB() = default;

  virtual Status Put(const WriteOptions& opts, const Slice& key, const Slice& value) = 0;
  virtual Status Delete(const WriteOptions& opts, const Slice& key) = 0;
  virtual Status Write(const WriteOptions& opts, const WriteBatch& batch) = 0;
  virtual Status Get(const ReadOptions& opts, const Slice& key, std::string* value) = 0;
  // Snapshot iterator over the state at creation time (tombstones hidden).
  virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& opts) = 0;

  // Flush the active memtable to L0 and wait for it.
  virtual Status Flush() = 0;
  // Block until no compaction is pending or running.
  virtual Status WaitForCompactions() = 0;
  // Force-compact every level down to the bottom.
  virtual Status CompactAll() = 0;

  [[nodiscard]] virtual DBStats GetStats() const = 0;
  // Non-OK once the engine has panicked (fsync failure) — all writes fail.
  [[nodiscard]] virtual Status BackgroundError() const = 0;
};

}  // namespace lsm
