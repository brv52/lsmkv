// Direct-I/O abstraction.
//
// Every file read/write issued by the engine (WAL, SSTables) goes through an
// IoBackend. All buffers, offsets and lengths must be multiples of
// kIoAlignment (4 KiB) because files are opened with O_DIRECT; the backends
// check this and fail with InvalidArgument rather than letting the kernel
// return a bare EINVAL.
//
// Backend choice is a runtime decision (Options::io_backend, kernel support,
// seccomp policy), so the interface is a classic virtual one: a single
// indirect call is noise next to a syscall, and CRTP would force the choice
// into the type system.
#pragma once

#include <sys/types.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "lsm/options.hpp"
#include "lsm/status.hpp"

namespace lsm {

inline constexpr size_t kIoAlignment = 4096;

constexpr uint64_t AlignUp(uint64_t v, uint64_t a = kIoAlignment) noexcept { return (v + a - 1) & ~(a - 1); }
constexpr uint64_t AlignDown(uint64_t v, uint64_t a = kIoAlignment) noexcept { return v & ~(a - 1); }
constexpr bool IsAligned(uint64_t v, uint64_t a = kIoAlignment) noexcept { return (v & (a - 1)) == 0; }
inline bool IsAligned(const void* p, uint64_t a = kIoAlignment) noexcept {
  return IsAligned(reinterpret_cast<uintptr_t>(p), a);
}

// Returned to a pool instead of freed (registered io_uring buffers).
class BufferRecycler {
 public:
  virtual ~BufferRecycler() = default;
  virtual void Recycle(char* data, int index) noexcept = 0;
};

// Owning, move-only, kIoAlignment-aligned byte buffer. Either heap memory from
// posix_memalign or a lease on a registered (fixed) io_uring buffer, in which
// case fixed_index() >= 0 and the backend uses READ_FIXED/WRITE_FIXED.
class AlignedBuffer {
 public:
  AlignedBuffer() noexcept = default;
  // Allocates AlignUp(capacity) bytes. Aborts on OOM (like operator new).
  static AlignedBuffer Allocate(size_t capacity, bool zero = false);
  static AlignedBuffer FromRecycler(char* data, size_t capacity, int index,
                                    std::shared_ptr<BufferRecycler> owner) noexcept {
    AlignedBuffer b;
    b.data_ = data;
    b.capacity_ = capacity;
    b.fixed_index_ = index;
    b.owner_ = std::move(owner);
    return b;
  }

  AlignedBuffer(AlignedBuffer&& o) noexcept { *this = std::move(o); }
  AlignedBuffer& operator=(AlignedBuffer&& o) noexcept {
    if (this != &o) {
      Reset();
      data_ = std::exchange(o.data_, nullptr);
      capacity_ = std::exchange(o.capacity_, 0);
      size_ = std::exchange(o.size_, 0);
      fixed_index_ = std::exchange(o.fixed_index_, -1);
      owner_ = std::move(o.owner_);
    }
    return *this;
  }
  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;
  ~AlignedBuffer() { Reset(); }

  [[nodiscard]] char* data() noexcept { return data_; }
  [[nodiscard]] const char* data() const noexcept { return data_; }
  [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
  // Logical (valid) byte count, maintained by the user.
  [[nodiscard]] size_t size() const noexcept { return size_; }
  void set_size(size_t n) noexcept { size_ = n; }
  [[nodiscard]] int fixed_index() const noexcept { return fixed_index_; }
  [[nodiscard]] explicit operator bool() const noexcept { return data_ != nullptr; }

  void Reset() noexcept {
    if (data_ != nullptr) {
      if (owner_) owner_->Recycle(data_, fixed_index_);
      else std::free(data_);
    }
    data_ = nullptr;
    capacity_ = size_ = 0;
    fixed_index_ = -1;
    owner_.reset();
  }

 private:
  char* data_ = nullptr;
  size_t capacity_ = 0;
  size_t size_ = 0;
  int fixed_index_ = -1;
  std::shared_ptr<BufferRecycler> owner_;
};

enum class IoOpcode : uint8_t { kRead, kWrite };

struct IoRequest {
  IoOpcode op = IoOpcode::kRead;
  int fd = -1;
  void* buf = nullptr;
  size_t len = 0;
  uint64_t offset = 0;
  int buf_index = -1;  // Registered buffer index, or -1.
  // Out: bytes transferred (reads may be short at EOF) or -errno.
  int64_t result = 0;
};

enum class SyncMode : uint8_t {
  kData,  // fdatasync(2)
  kFull,  // fsync(2)
};

struct IoStats {
  std::atomic<uint64_t> bytes_read{0};
  std::atomic<uint64_t> bytes_written{0};
  std::atomic<uint64_t> read_ops{0};
  std::atomic<uint64_t> write_ops{0};
  std::atomic<uint64_t> syncs{0};
  std::atomic<uint64_t> eagain_retries{0};
};

class IoBackend {
 public:
  IoBackend() = default;
  IoBackend(const IoBackend&) = delete;
  IoBackend& operator=(const IoBackend&) = delete;
  virtual ~IoBackend() = default;

  [[nodiscard]] virtual std::string Name() const = 0;

  // Fast-path registration (io_uring fixed files). Optional; a no-op for the
  // pread backend. Callers must unregister before closing the fd.
  virtual void RegisterFile(int /*fd*/) {}
  virtual void UnregisterFile(int /*fd*/) {}

  // Executes every request and returns once all have completed. Short
  // writes and EAGAIN/EINTR are retried internally; short reads are retried
  // until EOF. Each request's `result` is filled in. Returns the first error.
  virtual Status Submit(std::span<IoRequest> reqs) = 0;

  // Durability barrier. Always goes through the libc wrappers so LD_PRELOAD
  // fault injection (crash/eio_shim.c) and tracing (crash/tracer.cpp) observe
  // it. Any failure must be treated as fatal by the caller (fsyncgate).
  virtual Status Sync(int fd, SyncMode mode);

  // Returns an aligned buffer, preferring a registered fixed buffer when one
  // of sufficient size is free.
  virtual AlignedBuffer AllocateBuffer(size_t capacity) { return AlignedBuffer::Allocate(capacity); }

  // Convenience single-request wrappers.
  Status Read(int fd, void* buf, size_t len, uint64_t offset, size_t* bytes_read, int buf_index = -1);
  Status Write(int fd, const void* buf, size_t len, uint64_t offset, int buf_index = -1);

  [[nodiscard]] const IoStats& stats() const noexcept { return stats_; }

 protected:
  static Status ValidateRequest(const IoRequest& r);
  IoStats stats_;
};

// Backend factories. NewIoBackend honours Options::custom_backend and
// Options::io_backend (kAuto falls back to pread when io_uring is
// unavailable, logging the reason to stderr once).
std::shared_ptr<IoBackend> NewIoBackend(const Options& options, Status* status);
std::shared_ptr<IoBackend> NewPreadBackend();
std::shared_ptr<IoBackend> NewUringBackend(const Options& options, Status* status);

// open(2) wrapper implementing the O_DIRECT policy from Options. `*direct`
// reports whether O_DIRECT is actually in effect.
Status OpenFile(const std::string& path, int flags, mode_t mode, bool want_direct,
                bool allow_buffered_fallback, int* fd, bool* direct);

}  // namespace lsm
