// Backend-independent parts of the I/O layer.
#include "lsm/iobackend.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

namespace lsm {

AlignedBuffer AlignedBuffer::Allocate(size_t capacity, bool zero) {
  AlignedBuffer b;
  const size_t cap = AlignUp(capacity == 0 ? kIoAlignment : capacity);
  void* p = nullptr;
  if (::posix_memalign(&p, kIoAlignment, cap) != 0 || p == nullptr) throw std::bad_alloc();
  if (zero) std::memset(p, 0, cap);
  b.data_ = static_cast<char*>(p);
  b.capacity_ = cap;
  return b;
}

Status IoBackend::ValidateRequest(const IoRequest& r) {
  if (r.fd < 0) return Status::InvalidArgument("io request on invalid fd");
  if (r.buf == nullptr) return Status::InvalidArgument("io request with null buffer");
  if (!IsAligned(r.buf) || !IsAligned(r.offset) || !IsAligned(r.len)) {
    return Status::InvalidArgument("direct I/O requires 4 KiB aligned buffer, offset and length");
  }
  return Status::OK();
}

Status IoBackend::Sync(int fd, SyncMode mode) {
  stats_.syncs.fetch_add(1, std::memory_order_relaxed);
  // No retry loop, not even for EINTR: after a failed fsync the kernel may
  // already have marked the dirty pages clean (fsyncgate), so a second call
  // can "succeed" without the data ever reaching the device.
  const int rc = mode == SyncMode::kData ? ::fdatasync(fd) : ::fsync(fd);
  if (rc != 0) return Status::IOError(mode == SyncMode::kData ? "fdatasync" : "fsync", errno);
  return Status::OK();
}

Status IoBackend::Read(int fd, void* buf, size_t len, uint64_t offset, size_t* bytes_read, int buf_index) {
  IoRequest r;
  r.op = IoOpcode::kRead;
  r.fd = fd;
  r.buf = buf;
  r.len = len;
  r.offset = offset;
  r.buf_index = buf_index;
  Status s = Submit(std::span<IoRequest>(&r, 1));
  if (bytes_read != nullptr) *bytes_read = r.result > 0 ? static_cast<size_t>(r.result) : 0;
  return s;
}

Status IoBackend::Write(int fd, const void* buf, size_t len, uint64_t offset, int buf_index) {
  IoRequest r;
  r.op = IoOpcode::kWrite;
  r.fd = fd;
  r.buf = const_cast<void*>(buf);
  r.len = len;
  r.offset = offset;
  r.buf_index = buf_index;
  return Submit(std::span<IoRequest>(&r, 1));
}

std::shared_ptr<IoBackend> NewIoBackend(const Options& options, Status* status) {
  *status = Status::OK();
  if (options.custom_backend) return options.custom_backend;
  switch (options.io_backend) {
    case IoBackendKind::kPread:
      return NewPreadBackend();
    case IoBackendKind::kUring:
      return NewUringBackend(options, status);
    case IoBackendKind::kAuto: {
      Status s;
      auto b = NewUringBackend(options, &s);
      if (b) return b;
      static std::once_flag warned;
      std::call_once(warned, [&] {
        std::fprintf(stderr, "lsmkv: io_uring unavailable (%s); using preadv2/pwritev2 backend\n",
                     s.ToString().c_str());
      });
      return NewPreadBackend();
    }
  }
  *status = Status::InvalidArgument("unknown io backend");
  return nullptr;
}

Status OpenFile(const std::string& path, int flags, mode_t mode, bool want_direct,
                bool allow_buffered_fallback, int* fd, bool* direct) {
  flags |= O_CLOEXEC;
  if (want_direct) {
    const int f = ::open(path.c_str(), flags | O_DIRECT, mode);
    if (f >= 0) {
      *fd = f;
      if (direct) *direct = true;
      return Status::OK();
    }
    // EINVAL: the filesystem does not implement O_DIRECT (tmpfs, ...).
    if (errno != EINVAL || !allow_buffered_fallback) return Status::FromErrno("open(O_DIRECT) " + path);
  }
  const int f = ::open(path.c_str(), flags, mode);
  if (f < 0) {
    if (errno == ENOENT) return Status::NotFound(path);
    return Status::FromErrno("open " + path);
  }
  *fd = f;
  if (direct) *direct = false;
  return Status::OK();
}

}  // namespace lsm
