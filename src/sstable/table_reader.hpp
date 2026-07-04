// Read side of an SSTable, plus the cache of open tables.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "cache/lru_cache.hpp"
#include "lsm/db.hpp"
#include "lsm/iobackend.hpp"
#include "lsm/iterator.hpp"
#include "sstable/block.hpp"
#include "sstable/format.hpp"

namespace lsm::sst {

struct TableStats {
  std::atomic<uint64_t> bloom_checked{0};
  std::atomic<uint64_t> bloom_useful{0};
  std::atomic<uint64_t> blocks_read{0};
};

// Sequential read window shared by consecutive block fetches of one
// iterator (see ReadOptions::readahead_size).
struct ReadaheadBuffer {
  AlignedBuffer buf;
  uint64_t offset = 0;  // File offset of buf[0].
  size_t len = 0;       // Valid bytes.
};

class TableReader {
 public:
  // Reads the footer (last 4 KiB page), then pins the index and bloom blocks.
  static Status Open(const Options& options, IoBackend* io, LruCache* cache, TableStats* stats,
                     uint64_t file_number, const std::string& path, std::unique_ptr<TableReader>* out);
  ~TableReader();
  TableReader(const TableReader&) = delete;
  TableReader& operator=(const TableReader&) = delete;

  // Looks up the newest entry for ExtractUserKey(lookup_ikey) with sequence
  // <= the lookup's. *found is false if the table has no such entry.
  Status Get(const ReadOptions& ro, const Slice& lookup_ikey, bool* found, ValueType* type, std::string* value);

  // Iterates internal keys. The reader must outlive the iterator.
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator(const ReadOptions& ro) const;

  [[nodiscard]] bool BloomMayContain(const Slice& user_key) const noexcept;
  [[nodiscard]] uint64_t file_number() const noexcept { return file_number_; }
  [[nodiscard]] uint64_t file_size() const noexcept { return file_size_; }

  // Returns the data block for `handle`, from cache or disk.
  Status GetBlock(const ReadOptions& ro, const BlockHandle& handle, std::shared_ptr<const Block>* out) const;
  // Same, but cache misses are served from (and refill) `ra` in chunks of
  // ro.readahead_size.
  Status GetBlock(const ReadOptions& ro, const BlockHandle& handle, ReadaheadBuffer* ra,
                  std::shared_ptr<const Block>* out) const;

 private:
  TableReader(IoBackend* io, LruCache* cache, TableStats* stats, int fd, uint64_t file_number, uint64_t size);
  static Status ReadBlockFromFile(IoBackend* io, int fd, uint64_t file_size, const BlockHandle& h, bool verify,
                                  std::shared_ptr<const Block>* out);
  // Verifies the trailer of a block whose bytes start at buf[0].
  static Status ParseBlock(AlignedBuffer buf, const BlockHandle& h, bool verify, std::shared_ptr<const Block>* out);

  IoBackend* const io_;
  LruCache* const cache_;
  TableStats* const stats_;
  const int fd_;
  const uint64_t file_number_;
  const uint64_t file_size_;
  std::shared_ptr<const Block> index_;
  std::shared_ptr<const Block> bloom_;
  Slice bloom_data_;
};

// Keeps every live table open (fd + pinned index/filter). Thread-safe.
class TableCache {
 public:
  TableCache(const Options& options, std::string dbname, IoBackend* io, LruCache* block_cache, TableStats* stats);

  Status Get(uint64_t file_number, std::shared_ptr<TableReader>* out);
  // Drop the reader of a deleted file (readers already holding it are fine).
  void Evict(uint64_t file_number);

 private:
  const Options options_;
  const std::string dbname_;
  IoBackend* const io_;
  LruCache* const block_cache_;
  TableStats* const stats_;
  std::mutex mu_;
  std::unordered_map<uint64_t, std::shared_ptr<TableReader>> tables_;
};

}  // namespace lsm::sst
