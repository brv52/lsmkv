// WAL writer and the group-commit pipeline.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "lsm/db.hpp"
#include "lsm/iobackend.hpp"
#include "lsm/status.hpp"
#include "wal/log_format.hpp"

namespace lsm::wal {

// Appends records to one WAL file with 4 KiB aligned direct writes.
//
// The writer keeps the partially filled tail block in an aligned staging
// buffer. Each AddGroup() serialises the group after it and writes the range
// [tail block start, padded end) in one request, so the tail block is
// rewritten by the next commit unless Options::wal_pad_commits is set.
// Not thread-safe: only the group-commit leader touches it.
class LogWriter {
 public:
  struct BatchRef {
    const WriteBatch* batch;
    uint64_t first_seq;
  };

  ~LogWriter();
  LogWriter(const LogWriter&) = delete;
  LogWriter& operator=(const LogWriter&) = delete;

  // Creates (O_EXCL) and preallocates the log file.
  static Status Create(IoBackend* io, const std::string& path, uint64_t log_number, const Options& options,
                       std::unique_ptr<LogWriter>* out);

  // Serialises every batch. With `sync` the whole buffer is written and
  // fdatasynced. Without it, only complete blocks are written when
  // Options::wal_defer_unsynced_writes is set (the partial tail waits for the
  // next sync/close), otherwise everything is written but not synced.
  Status AddGroup(std::span<const BatchRef> batches, bool sync);
  // Writes any buffered bytes, then fdatasyncs.
  Status Sync();
  // Durably closes the file (fdatasync unless already clean).
  Status Close();
  // Drops the descriptor without syncing (panic path).
  void Abandon() noexcept;

  [[nodiscard]] uint64_t log_number() const noexcept { return log_number_; }
  [[nodiscard]] uint64_t logical_size() const noexcept { return file_off_ + buf_len_; }
  [[nodiscard]] uint64_t bytes_submitted() const noexcept { return bytes_submitted_; }
  [[nodiscard]] bool direct_io() const noexcept { return direct_; }

  // Encodes a single record into dst (exposed for tests and the reader).
  static void EncodeRecord(RecordType type, const Slice& key, const Slice& value, std::string* dst);

 private:
  LogWriter(IoBackend* io, int fd, bool direct, uint64_t log_number, const Options& options);

  void EnsureCapacity(size_t extra);
  void AppendRecord(RecordType type, const Slice& key, const Slice& value);
  void AppendBytes(const char* p, size_t n);
  void PadToBlockBoundary();
  Status FlushBuffer();
  Status FlushFullBlocks();

  IoBackend* const io_;
  int fd_;
  const bool direct_;
  const uint64_t log_number_;
  const bool pad_commits_;
  const bool defer_unsynced_;
  AlignedBuffer buf_;
  uint64_t file_off_ = 0;  // File offset of buf_[0]; always block aligned.
  size_t buf_len_ = 0;     // Valid bytes in buf_.
  bool dirty_ = false;      // Written since the last successful sync.
  bool unwritten_ = false;  // buf_ holds bytes not yet submitted to the file.
  uint64_t bytes_submitted_ = 0;
};

// Lock-free writer queue implementing pipelined group commit.
//
// Writers push themselves onto an intrusive Treiber stack with a single CAS.
// The writer that finds the stack empty becomes leader; it collects every
// writer queued at that instant (bounded by max_group_bytes), assigns
// consecutive sequence numbers, and runs `log` (WAL append + fdatasync) once
// for the whole group. It then hands leadership to the next queued writer
// *before* the memtable inserts happen, so group N+1's WAL write overlaps
// with group N's inserts. Every member applies its own batch concurrently
// (the memtable is a concurrent skiplist); the last one to finish publishes
// the group's sequence range in commit order and wakes everyone.
class GroupCommitQueue {
 public:
  struct Group;

  struct Writer {
    const WriteBatch* batch = nullptr;
    bool sync = false;
    bool disable_wal = false;
    // Out.
    uint64_t first_seq = 0;
    Status status;

   private:
    friend class GroupCommitQueue;
    enum State : uint8_t { kInit = 0, kLeader = 1, kLogged = 2, kCompleted = 4 };
    std::atomic<uint8_t> state{kInit};
    std::mutex mu;
    std::condition_variable cv;
    Writer* link_older = nullptr;
    Writer* link_newer = nullptr;
    Group* group = nullptr;
  };

  struct Group {
    std::vector<Writer*> writers;  // Commit (FIFO) order; writers[0] is the leader.
    uint64_t first_seq = 0;
    uint64_t last_seq = 0;  // first_seq - 1 when the group carries no ops.
    bool sync = false;
    std::shared_ptr<void> context;  // Filled by `log`, read by `apply` (e.g. the target memtable).
    std::atomic<size_t> pending{0};
  };

  // Called by the leader only, strictly one group at a time.
  using LogFn = std::function<Status(Group&)>;
  // Called concurrently by every member of a successfully logged group.
  using ApplyFn = std::function<Status(Writer&, Group&)>;

  GroupCommitQueue(LogFn log, ApplyFn apply, size_t max_group_bytes);

  // Blocks until w's batch is durable (if requested), applied and visible.
  Status Commit(Writer* w);

  // Sequence numbers <= LastPublished() are fully applied and visible.
  [[nodiscard]] uint64_t LastPublished() const noexcept { return published_.load(std::memory_order_acquire); }
  // Highest sequence handed out (only stable while no writes are in flight).
  [[nodiscard]] uint64_t LastAllocated() const noexcept { return allocated_.load(std::memory_order_acquire); }
  // Initialise after recovery, before any Commit().
  void SetLastSequence(uint64_t seq) noexcept;
  // Wait until every sequence <= seq has been published.
  void WaitForPublished(uint64_t seq) const noexcept;

  [[nodiscard]] uint64_t groups() const noexcept { return groups_.load(std::memory_order_relaxed); }
  [[nodiscard]] uint64_t grouped_writers() const noexcept { return grouped_writers_.load(std::memory_order_relaxed); }

 private:
  void LeadGroup(Writer* leader);
  void FinishApply(Writer* self);
  static void Await(Writer* w, uint8_t mask);
  static void Wake(Writer* w, uint8_t state);

  const LogFn log_;
  const ApplyFn apply_;
  const size_t max_group_bytes_;
  std::atomic<Writer*> newest_{nullptr};
  std::atomic<uint64_t> allocated_{0};  // Written by the leader only.
  std::atomic<uint64_t> published_{0};
  std::atomic<uint64_t> groups_{0};
  std::atomic<uint64_t> grouped_writers_{0};
};

}  // namespace lsm::wal
