// Engine core: write pipeline, background flush/compaction, read path,
// recovery and panic handling.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "cache/lru_cache.hpp"
#include "compaction/compaction.hpp"
#include "lsm/db.hpp"
#include "lsm/iobackend.hpp"
#include "manifest/version_set.hpp"
#include "memtable/memtable.hpp"
#include "sstable/table_reader.hpp"
#include "util/file_util.hpp"
#include "wal/log_writer.hpp"

namespace lsm {

class DBImpl final : public DB {
 public:
  DBImpl(const Options& options, std::string dbname, std::shared_ptr<IoBackend> io);
  ~DBImpl() override;

  Status Recover();
  void StartBackgroundThreads();

  Status Put(const WriteOptions& opts, const Slice& key, const Slice& value) override;
  Status Delete(const WriteOptions& opts, const Slice& key) override;
  Status Write(const WriteOptions& opts, const WriteBatch& batch) override;
  Status Get(const ReadOptions& opts, const Slice& key, std::string* value) override;
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& opts) override;
  Status Flush() override;
  Status WaitForCompactions() override;
  Status CompactAll() override;
  [[nodiscard]] DBStats GetStats() const override;
  [[nodiscard]] Status BackgroundError() const override;

 private:
  using Group = wal::GroupCommitQueue::Group;
  using Writer = wal::GroupCommitQueue::Writer;

  // Group-commit callbacks.
  Status LogGroup(Group& g);
  Status ApplyWriter(Writer& w, Group& g);
  // REQUIRES: mu_ held via lk; caller is the group-commit leader.
  Status MakeRoomForWrite(std::unique_lock<std::mutex>& lk, Group& g);

  void FlushThread();
  void CompactionThread();
  Status FlushMemTable(const std::shared_ptr<MemTable>& imm);
  Status DoCompaction(Compaction* c);
  bool NeedsCompaction() const;
  void DeleteObsoleteFiles();

  // Records an unrecoverable error. For I/O failures in the durability path
  // (fsyncgate) this aborts the process when Options::abort_on_fsync_error.
  void Panic(const Status& cause, const char* where);
  [[nodiscard]] bool panicked() const noexcept { return panicked_.load(std::memory_order_acquire); }

  const Options options_;
  const std::string dbname_;
  const std::shared_ptr<IoBackend> io_;
  std::unique_ptr<LruCache> block_cache_;
  sst::TableStats table_stats_;
  std::unique_ptr<sst::TableCache> table_cache_;
  std::unique_ptr<VersionSet> versions_;
  std::unique_ptr<wal::GroupCommitQueue> queue_;
  ScopedFd db_lock_;

  mutable std::mutex mu_;
  std::condition_variable bg_cv_;     // Wakes background threads.
  std::condition_variable state_cv_;  // Flush/compaction progress (stalled writers, waiters).
  std::shared_ptr<MemTable> mem_;
  std::deque<std::shared_ptr<MemTable>> imm_;  // Oldest first.
  std::unique_ptr<wal::LogWriter> log_;        // Touched only by the group-commit leader after open.
  std::set<uint64_t> pending_outputs_;
  Status bg_error_;
  bool compaction_running_ = false;

  std::mutex compaction_mu_;  // Held by whoever runs a compaction (bg thread or CompactAll).
  std::atomic<bool> panicked_{false};
  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> force_switch_{false};
  std::thread flush_thread_;
  std::thread compaction_thread_;

  std::atomic<uint64_t> user_bytes_{0};
  std::atomic<uint64_t> wal_bytes_{0};
  std::atomic<uint64_t> flush_bytes_{0};
  std::atomic<uint64_t> compaction_bytes_written_{0};
  std::atomic<uint64_t> compaction_bytes_read_{0};
  std::atomic<uint64_t> write_stalls_{0};
};

}  // namespace lsm
