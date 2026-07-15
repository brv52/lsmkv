# lsmkv design

lsmkv is a leveled LSM-tree key-value store for Linux that does all file I/O
with `O_DIRECT` through a pluggable `IoBackend` (io_uring or preadv2/pwritev2).
This document describes the on-disk formats, the write and read pipelines, the
durability protocol, and how crash consistency is verified.

```
            Put/Delete/Write                         Get / NewIterator
                  │                                         │
   ┌──────────────▼───────────────┐            ┌────────────▼────────────┐
   │ GroupCommitQueue (lock-free) │            │ snapshot = LastPublished │
   │  leader: MakeRoom → WAL      │            │ mem → imm[] → L0 → L1..  │
   │  append → fdatasync          │            └────────────┬────────────┘
   │  members: concurrent insert  │                         │
   └──────┬───────────────┬───────┘              ┌──────────▼──────────┐
          │               │                      │ LruCache (sharded)  │
   ┌──────▼─────┐  ┌──────▼──────────┐           │ (file#, offset) →   │
   │ LogWriter  │  │ MemTable        │           │ shared_ptr<Block>   │
   │ O_DIRECT   │  │ (CAS skiplist)  │           └──────────┬──────────┘
   └──────┬─────┘  └──────┬──────────┘                      │ miss: 1 aligned read
          │        switch │ (size)                          │ straight into the
          │        ┌──────▼──────┐  flush thread   ┌────────▼──────────┐ cached buffer
          │        │ imm memtable├────────────────►│ L0 SSTables       │
          │        └─────────────┘                 ├───────────────────┤
          │                         compaction     │ L1 … Ln (T = 10)  │
          │                         thread ───────►│ non-overlapping   │
          ▼                                        └───────────────────┘
   IoBackend: UringBackend (sharded rings, fixed buffers/files) | PreadBackend
```

## Source map

| Area | Files |
|---|---|
| Public API | `include/lsm/{db,options,slice,status,iterator,iobackend}.hpp` |
| I/O backends | `src/io/iobackend.cpp`, `src/io/uring.{hpp,cpp}`, `src/io/pread.{hpp,cpp}` |
| WAL + group commit | `src/wal/log_format.hpp`, `src/wal/log_writer.{hpp,cpp}`, `src/wal/log_reader.{hpp,cpp}` |
| Memtable | `src/memtable/{arena,skiplist,memtable}.hpp`, `memtable.cpp` |
| SSTables | `src/sstable/{format,block,block_builder,bloom,table_builder,table_reader}.*` |
| Block cache | `src/cache/lru_cache.{hpp,cpp}` |
| Manifest | `src/manifest/{version_edit,version_set}.hpp`, `version_set.cpp` |
| Compaction | `src/compaction/compaction.{hpp,cpp}` |
| Engine core | `src/db_impl.{hpp,cpp}` |
| Utilities | `src/util/{coding,crc32c,hash,dbformat,file_util}.*` |
| Crash harness | `crash/{eio_shim.c,tracer.cpp,replay.cpp}` |
| Benchmarks | `bench/kvbench.cpp`, `bench/rocksdb_dbbench.sh`, `bench/fio/*.fio` |

## Alignment rules

Every byte that reaches a WAL or SSTable goes through `IoBackend::Submit` with
buffer address, file offset and length all multiples of 4 KiB
(`kIoAlignment`). The backends reject anything else with `InvalidArgument`
instead of surfacing the kernel's bare `EINVAL`. Buffers come from
`AlignedBuffer` (`posix_memalign`, or a lease on a registered io_uring buffer).
Files are always a whole number of pages long: the WAL is written in padded
pages, and SSTables pad every block and put the footer in the last 48 bytes of
the final page.

If the filesystem refuses `O_DIRECT` (tmpfs, some overlayfs setups) and
`Options::allow_buffered_fallback` is set, files are opened buffered and the
same alignment rules still apply.

## Write-ahead log

Record (`src/wal/log_format.hpp`):

```
CRC32C (4B, masked) | Payload Length (4B) | Type (1B) | Key Length (2B) | Key | Value
```

The CRC covers everything after itself. Payload length is key + value. Types
are `kPut`, `kDelete`, `kPadding` and `kBatchHeader`. Every `WriteBatch` is
framed by a `kBatchHeader` record (`fixed64 first_seq | fixed32 count`), and
recovery applies a batch only if all `count` records are intact, so batches
stay atomic across torn writes. A record never starts in the last 10 bytes of
a 4 KiB block. Those bytes are zero filler, which keeps headers from straddling
pages. A zero header marks the logical end of the log, so unwritten or
preallocated (`fallocate`) space reads as EOF.

**Tail handling.** `LogWriter` keeps the partially filled tail page in an
aligned staging buffer. A synced commit writes `[tail page start, padded end)`
in a single request, which means the next commit rewrites that page. If your
device's atomic write unit is smaller than 4 KiB, a torn rewrite could damage
records that were already acknowledged. For that case set
`Options::wal_pad_commits`: every commit is padded to a page boundary with a
`kPadding` record, and no acknowledged sector is ever rewritten.

`Options::wal_defer_unsynced_writes` (on by default) makes unsynced commits
write only complete pages. Without it, every 100-byte unsynced write rewrites a
4 KiB page: 31× WAL amplification measured, versus 1.24× with deferral. Under
`O_DIRECT` there's no page cache to fall back on, so deferred bytes can be lost
when the *process* crashes. That is within `sync = false`'s contract, but it is
stricter than a buffered WAL.

CRC32C uses the SSE4.2 `crc32` instructions (`__builtin_ia32_crc32{di,si,qi}`),
selected at startup with `__builtin_cpu_supports`, and falls back to a
slicing-by-8 table.

## Group commit (pipelined)

`wal::GroupCommitQueue` is an intrusive Treiber stack of writers.

1. **Join.** A writer pushes itself with one CAS. If the stack was empty it
   becomes the leader; otherwise it waits (a short spin, then its own
   mutex/condvar).
2. **Lead.** The leader walks from the newest writer down to itself, forms a
   FIFO group (capped at `wal_max_group_bytes`) and assigns consecutive
   sequence numbers. It then runs `LogGroup` once for the whole group:
   `MakeRoomForWrite` (stalls, memtable/WAL switch), one aligned WAL write,
   and one `fdatasync` if any member asked for `sync`.
3. **Hand off.** Leadership passes to the next queued writer *before* the
   memtable inserts. That's the pipelining: group N+1's WAL write and fsync
   overlap with group N's inserts.
4. **Apply.** Every member inserts its own batch into the concurrent skiplist
   in parallel.
5. **Publish.** The last member to finish waits until the previous group has
   published, sets `published = last_seq`, and wakes the group, leader last
   (the group lives in the leader's stack frame). Readers use `published` as
   their snapshot, so a sequence range only becomes visible once it is fully
   applied, and always in commit order.

If `LogGroup` fails, the group's sequence numbers are rolled back (nothing was
applied) and every member gets the error.

Measured on this development box (WSL2, fdatasync ≈ 1.9 ms): the queue unit test
puts 8.1 writers in each group, and the DB-level concurrent test does 2400
synced commits in 543 fdatasyncs.

## Memtable

An arena-backed, insert-only, lock-free skiplist (`src/memtable/skiplist.hpp`).
A node is linked bottom-up with one CAS per level. Level 0 is the
linearization point; upper levels are only shortcuts. When a CAS fails, the
splice for that level is recomputed starting from the old predecessor, which
is still less than the key because nothing is ever deleted. The arena's fast
path is a single `fetch_add` on the current 1 MiB block.

Entry encoding: `varint32(klen+8) | user_key | fixed64(seq << 8 | type) | varint32(vlen) | value`.
Internal keys sort by user key ascending, then sequence descending.

Once the arena has handed out `write_buffer_size` bytes, the leader seals the
memtable and records the last sequence assigned to it. It then syncs and closes
the old WAL, opens a new one (dirent fsynced before any ack), and wakes the
flush thread. Before building the L0 table, the flush thread waits until that
sequence is published, because pipelined inserts may still be running.

## SSTables

```
[data block][pad] … [index block][pad] [bloom block][pad][48-byte footer]
footer = index(off,size) | bloom(off,size) | fixed32 version | fixed32 crc | fixed64 magic
```

* Data blocks use LevelDB-style prefix compression with restart points every
  16 keys. A block is cut *before* an entry would push it past 4 KiB including
  the 5-byte trailer (`type | masked crc32c`), so a typical block is exactly
  one page, read with exactly one aligned I/O.
* Index entries map each block's last internal key to its handle.
* Bloom filter: cache-local blocked bloom. MurmurHash3 x64 picks one 64-byte
  line, and k = 6 probes stay inside it. At 10 bits/key the measured false
  positive rate is 0.997% with one cache miss per probe.
* `TableBuilder` stages output in a 1 MiB aligned buffer (a fixed io_uring
  buffer when one is free) and ends with `fdatasync`. The directory is
  fsynced before the manifest references the new file.

## Block cache

Because `O_DIRECT` bypasses the page cache, `LruCache` is the only cache.
There are 2^4 shards by default, each with its own mutex, an intrusive list
and a hash map keyed by `(file_number, block_offset)`. Values are
`shared_ptr<const Block>`: a reader pins a block simply by holding the
pointer, and eviction only drops the cache's own reference. The block *is* the
aligned buffer the DMA landed in, so there's no copy between device and cache.
Compaction reads with `fill_cache = false` and a 2 MiB readahead window
(`compaction_readahead_size`). Kernel readahead doesn't exist under
`O_DIRECT`, and on the test box a 4 KiB direct read costs ≈ 90 µs versus
≈ 760 µs for 1 MiB, so batching reads matters.

## Manifest

`VersionSet::LogAndApply` writes a complete snapshot of the new version every
time:

1. write `MANIFEST-<n>` and `fsync` it;
2. write `<n>.tmp` containing `"MANIFEST-<n>\n"` and `fsync` it;
3. `renameat2(<n>.tmp, CURRENT)`, an atomic replace;
4. `fsync` the database directory;
5. unlink the previous manifest.

A crash at any point leaves `CURRENT` naming a complete, synced manifest
(the old one or the new one). Snapshots are small (one entry per live file),
and writing one per flush or compaction keeps recovery to a single read.

Recovery reads `CURRENT`, then the manifest. It marks every file number found
in the directory as used, because WALs are created without a manifest write.
It replays every WAL ≥ `log_number` into memtables, flushes those to L0,
starts a fresh WAL, and commits a manifest naming it. Only after that are the
replayed logs deleted.

## Compaction

Leveled, T = `max_bytes_for_level_multiplier` = 10. Scores:

* L0: `files / level0_compaction_trigger`
* Li: `bytes(Li) / (max_bytes_for_level_base × T^(i-1))`

The single compaction thread picks the highest score ≥ 1. L0 takes every L0
file; Li takes the next file after the level's compact pointer
(round-robin). Both add the overlapping files from Li+1. A single input with
nothing beneath it becomes a trivial move (metadata only). Otherwise a
min-heap k-way `MergingIterator` streams all inputs. The newest version of
each user key is kept; tombstones are dropped once no deeper level can hold
the key (`IsBaseLevelForKey`). Outputs are cut at `target_file_size`.
Writers slow down at `level0_slowdown_writes_trigger` and stop at
`level0_stop_writes_trigger` or when `max_immutable_memtables` is reached.

A file is deleted only when no live `Version` (readers, iterators, running
compactions all hold `shared_ptr<const Version>`) and no pending output
references it. `DeleteObsoleteFiles` lists the directory *before* it
snapshots pending outputs and live versions. The harness found the reverse
order deleting a freshly written compaction output.

## fsyncgate

From "Can Applications Recover from fsync Failures?" (Rebello et al., ATC '20)
and the 2018 PostgreSQL thread: when `fsync` fails, Linux may already have
marked the dirty pages clean, so a retry can "succeed" without the data ever
reaching the disk.

lsmkv's rules:

* `IoBackend::Sync` calls `fsync`/`fdatasync` exactly once and never retries,
  not even on `EINTR`.
* Every sync or write failure in the durability path (WAL append or sync, WAL
  switch, table build, directory fsync, manifest write/rename/fsync) goes to
  `DBImpl::Panic`.
* With `abort_on_fsync_error` (the default) `Panic` calls `std::abort()`
  **before publishing the error**, so no thread can observe the failure and
  keep going. The crash harness caught a version that published first: a
  caller of `Flush()` returned an error and exited normally before the abort
  happened.
* With `abort_on_fsync_error = false` (tests only), the WAL descriptor is
  closed without syncing, `bg_error_` becomes a sticky `Status::Panic`, every
  mutation fails, and reads keep working.
* Recovery trusts only what is durable: the WAL up to the first malformed
  record and the manifest named by `CURRENT`.

Durability barriers always go through libc, never `IORING_OP_FSYNC`, so the
`LD_PRELOAD` shim and tracer can see them.

## Crash-consistency verification

`crash_replay` drives a single-threaded child workload (re-exec of itself)
that reports `B <op> …` before each operation and `A <op>` after it is
acknowledged. It uses small buffers so ~300 operations exercise memtable
switches, flushes, manifest rewrites and multi-level compaction. After every
recovery, two invariants are checked:

1. every acknowledged write is present with the right value (only the single
   in-flight operation may go either way);
2. no key or value appears that was never written (checked with both `Get`
   and a full iterator scan).

| Mode | What it does |
|---|---|
| `eio` | `LD_PRELOAD=libeio_shim.so INJECT_EIO_AFTER_COUNT=N` for N = 0…24, then every 9th up to the end of the workload; `EIO` and `ENOSPC` are mixed. The child must die with `SIGABRT` (or fail `Open`), then recovery must satisfy both invariants. |
| `kill` | `SIGKILL` after a random number of acknowledgements. |
| `trace` | `LD_PRELOAD=liblsmkv_tracer.so` records every create/write/truncate/extend/fsync/rename/unlink under the DB directory, with ack marks interleaved. For each crash point k the trace is materialized two ways: **prefix** (ops [0,k) all applied) and **durable** (only ops made durable by a later sync before k: data ops need an fsync of the same file, namespace ops an fsync of the directory). Recovery runs on each state. |

Results on the development box:

* eio: 66 runs, 0 failures
* kill: 15 rounds, 0 failures
* trace: 496 ops, 217 syncs, 130 crash points, 260 recoveries, 0 failures

The harness is mutation-tested. Acknowledging WAL commits without `fdatasync`
produces 44 invariant-1 violations in the durable model.

The trace mode forces the pread backend because io_uring submissions bypass
libc. The `eio` mode works with either backend.

## Testing

* Unit tests (`tests/`, gtest) cover CRC32C vectors, backend alignment and
  concurrency, WAL round-trips and torn tails in both tail modes, group-commit
  ordering and rollback, skiplist concurrency, block/bloom/table/cache
  behaviour, full DB recovery, compaction, concurrent readers and writers,
  in-process fsync failure via a faulty backend, locking, and a corrupt
  `CURRENT`. Backend tests run for pread and io_uring (skipped when io_uring
  is unavailable).
* Sanitizers: `-DLSMKV_SANITIZER=address+undefined` and `=thread`. Both are
  clean on the full unit suite, and the crash harness also runs under ASan.

## Known limitations

* No snapshots API beyond iterator-at-creation, no compression, no reverse
  iteration, a single compaction thread.
* The trace persistence model is per-path. It doesn't model filesystems that
  reorder data writes *within* a single fsync interval differently from the
  prefix and durable extremes. These are the two models ALICE calls "prefix"
  and "sync-only".
* Recovery stops at the first malformed WAL record. A genuinely corrupt
  record in the middle of a log, followed by valid records, is treated as the
  end of the log, not reported as an error.
