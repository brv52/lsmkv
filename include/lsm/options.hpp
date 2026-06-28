// Engine configuration.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lsm {

class IoBackend;

enum class IoBackendKind : uint8_t {
  // Try io_uring first; if the kernel or a seccomp policy rejects
  // io_uring_setup() (EPERM / ENOSYS, e.g. default Docker profile), fall back
  // to the preadv2/pwritev2 backend. See docs/io_uring-notes.md.
  kAuto,
  kUring,
  kPread,
};

struct Options {
  // ---- Open behaviour ----
  bool create_if_missing = true;
  bool error_if_exists = false;

  // ---- I/O ----
  IoBackendKind io_backend = IoBackendKind::kAuto;
  // Inject a pre-built backend (tests, fault injection). Overrides io_backend.
  std::shared_ptr<IoBackend> custom_backend;
  // Open WAL and SSTables with O_DIRECT. When the filesystem rejects O_DIRECT
  // (tmpfs, some overlayfs setups -> EINVAL) and allow_buffered_fallback is
  // true, files are opened buffered instead; alignment rules still apply.
  bool use_direct_io = true;
  bool allow_buffered_fallback = true;
  unsigned uring_queue_depth = 256;
  unsigned uring_num_rings = 4;          // Rings are sharded by thread to avoid a global lock.
  unsigned uring_fixed_buffers = 16;     // Registered (fixed) buffers per ring, best effort.
  size_t uring_fixed_buffer_size = 256 * 1024;
  bool uring_register_files = true;
  bool uring_sqpoll = false;

  // ---- Write path ----
  size_t write_buffer_size = 64ull << 20;  // Memtable size that triggers a switch.
  int max_immutable_memtables = 2;         // Writers stall above this.
  // Pad every group commit to the next 4 KiB boundary instead of rewriting
  // the partially filled tail block on the next commit. Costs space and
  // bandwidth, but never overwrites a sector holding acknowledged records,
  // which matters on devices whose atomic write unit (AWUPF) is < 4 KiB.
  bool wal_pad_commits = false;
  // Unsynced commits (WriteOptions::sync = false) only write completed 4 KiB
  // blocks; the partial tail is written by the next synced commit, memtable
  // switch or close. Without this every unsynced commit rewrites a full tail
  // block (4 KiB for a 100-byte record). Note that with O_DIRECT there is no
  // page cache to fall back on, so with this enabled unsynced writes can be
  // lost on a *process* crash, not only on power loss.
  bool wal_defer_unsynced_writes = true;
  size_t wal_preallocate_bytes = 64ull << 20;
  size_t wal_max_group_bytes = 4ull << 20;

  // ---- fsyncgate ----
  // On any fsync/fdatasync failure the engine closes the affected file
  // descriptors and enters panic mode. If true it then terminates the
  // process (std::abort) so the only way forward is a clean recovery from the
  // last durable state. Never set false in production; it exists for tests
  // that want to observe the panic status in-process.
  bool abort_on_fsync_error = true;

  // ---- SSTables / read path ----
  size_t block_size = 4096;  // Target data block size; blocks are 4 KiB aligned.
  int block_restart_interval = 16;
  int bloom_bits_per_key = 10;
  size_t block_cache_capacity = 256ull << 20;
  int block_cache_shards_bits = 4;
  size_t target_file_size = 64ull << 20;
  // O_DIRECT disables kernel readahead, so compaction inputs are read in
  // chunks of this size instead of one 4 KiB block per syscall.
  size_t compaction_readahead_size = 2ull << 20;

  // ---- Compaction ----
  int num_levels = 7;
  int level0_compaction_trigger = 4;
  int level0_slowdown_writes_trigger = 12;
  int level0_stop_writes_trigger = 20;
  uint64_t max_bytes_for_level_base = 256ull << 20;
  double max_bytes_for_level_multiplier = 10.0;  // T
  bool disable_auto_compactions = false;
};

struct WriteOptions {
  // fdatasync the WAL before acknowledging. With group commit the cost is
  // amortised across every writer in the batch.
  bool sync = true;
  // Skip the WAL entirely (data lost on crash until flushed).
  bool disable_wal = false;
};

struct ReadOptions {
  bool verify_checksums = true;
  bool fill_cache = true;
  // Iterators: fetch this many bytes per read when blocks miss the cache
  // (0 = one block per read). Useful for long scans under O_DIRECT.
  size_t readahead_size = 0;
};

}  // namespace lsm
