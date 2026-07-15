# lsmkv

A crash-consistent leveled LSM-tree key-value store in C++20 for Linux.
All file I/O is 4 KiB-aligned `O_DIRECT` through a pluggable backend
(io_uring or `preadv2`/`pwritev2`).

- **WAL** with pipelined lock-free group commit, hardware CRC32C and atomic batches.
- **Memtable**: lock-free concurrent skiplist.
- **SSTables**: 4 KiB data blocks, blocked bloom filter (~1% FPR), sharded LRU block cache.
- **Manifest**: atomic replace (fsync → `renameat2` → dir fsync).
- **Compaction**: leveled, T = 10, score-based, k-way merge.
- **fsync failures** abort the process and are never retried (fsyncgate).
- **Crash harness**: EIO injection, `SIGKILL`, and syscall-trace replay.

## Build

```sh
sudo apt install liburing-dev libgtest-dev   # optional; see below
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

| CMake option | Default | Meaning |
|---|---|---|
| `LSMKV_FETCH_DEPS` | `ON` | Download liburing / googletest when not installed |
| `LSMKV_REQUIRE_URING` | `OFF` | Fail configuration instead of building without io_uring |
| `LSMKV_SANITIZER` | empty | `address`, `undefined`, `address+undefined` or `thread` |

Targets: `lsmkv_static`, `lsmkv_shared`, `kvbench`, `test_*`, `crash_replay`,
`eio_shim` (an `LD_PRELOAD` module) and `lsmkv_tracer`.

## Usage

```cpp
#include "lsm/db.hpp"

lsm::Options opt;
opt.create_if_missing = true;
std::unique_ptr<lsm::DB> db;
lsm::Status s = lsm::DB::Open(opt, "/data/db", &db);
s = db->Put({}, "key", "value");          // WriteOptions::sync = true by default
std::string v;
s = db->Get({}, "key", &v);
```

## Crash testing

```sh
./build/crash_replay eio   --dir /tmp/crash   # inject EIO/ENOSPC at each fsync
./build/crash_replay kill  --dir /tmp/crash   # SIGKILL at random points
./build/crash_replay trace --dir /tmp/crash   # replay recorded syscalls to every crash point
```

## Benchmarks

```sh
./build/kvbench --benchmarks=fillrandom,readrandom,overwrite --num=1000000 --db=/mnt/nvme/kv
bench/rocksdb_dbbench.sh                       # same workload vs RocksDB v11.8.1, with write amplification
FILENAME=/dev/nvme0n1 fio bench/fio/nvme_randread_qd1.fio
```

## Docs

- [docs/design.md](docs/design.md): formats, write/read paths, durability protocol, verification.
- [docs/io_uring-notes.md](docs/io_uring-notes.md): kernel requirements, Docker/Kubernetes seccomp profiles, `RLIMIT_MEMLOCK`.

## Status

The io_uring backend (`src/io/uring.cpp`) has not yet been compiled or tested
in the development environment, because liburing was unavailable there. The
pread backend is fully tested, including under ASan, UBSan and TSan.
