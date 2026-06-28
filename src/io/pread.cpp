#include "io/pread.hpp"

#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>

namespace lsm {

ssize_t PreadBackend::TransferOnce(IoOpcode op, int fd, char* buf, size_t len, uint64_t off) {
  const bool is_read = op == IoOpcode::kRead;
  auto& nowait = is_read ? read_nowait_ : write_nowait_;
  const auto offset = static_cast<off_t>(off);

  if (have_v2_.load(std::memory_order_relaxed)) {
    iovec iov{buf, len};
    if (nowait.load(std::memory_order_relaxed)) {
      const ssize_t n = is_read ? ::preadv2(fd, &iov, 1, offset, RWF_NOWAIT)
                                : ::pwritev2(fd, &iov, 1, offset, RWF_NOWAIT);
      if (n >= 0) return n;
      if (errno == EAGAIN) {
        stats_.eagain_retries.fetch_add(1, std::memory_order_relaxed);
        if (!is_read) nowait.store(false, std::memory_order_relaxed);
      } else if (errno == EOPNOTSUPP) {
        nowait.store(false, std::memory_order_relaxed);
      } else if (errno == ENOSYS) {
        have_v2_.store(false, std::memory_order_relaxed);
      } else {
        return -1;
      }
    }
    if (have_v2_.load(std::memory_order_relaxed)) {
      return is_read ? ::preadv2(fd, &iov, 1, offset, 0) : ::pwritev2(fd, &iov, 1, offset, 0);
    }
  }
  return is_read ? ::pread(fd, buf, len, offset) : ::pwrite(fd, buf, len, offset);
}

int64_t PreadBackend::Execute(IoRequest& r) {
  auto* buf = static_cast<char*>(r.buf);
  size_t done = 0;
  while (done < r.len) {
    const ssize_t n = TransferOnce(r.op, r.fd, buf + done, r.len - done, r.offset + done);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN) {  // Blocking call reported EAGAIN (e.g. O_NONBLOCK fd).
        stats_.eagain_retries.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      return -errno;
    }
    if (n == 0) {
      if (r.op == IoOpcode::kRead) break;  // EOF
      return -EIO;                         // Zero-length write: treat as device failure.
    }
    done += static_cast<size_t>(n);
    if (!IsAligned(static_cast<uint64_t>(n)) && done < r.len) {
      // A direct transfer stopped mid-block. For reads that is EOF (the next
      // offset would be unaligned anyway); for writes it is a hard error
      // (typically ENOSPC on the following block).
      if (r.op == IoOpcode::kRead) break;
      return -ENOSPC;
    }
  }
  return static_cast<int64_t>(done);
}

Status PreadBackend::Submit(std::span<IoRequest> reqs) {
  Status first_error;
  for (auto& r : reqs) {
    if (Status v = ValidateRequest(r); !v.ok()) {
      r.result = -EINVAL;
      if (first_error.ok()) first_error = v;
      continue;
    }
    r.result = Execute(r);
    if (r.result < 0) {
      if (first_error.ok()) {
        first_error = Status::IOError(r.op == IoOpcode::kRead ? "preadv2" : "pwritev2",
                                      static_cast<int>(-r.result));
      }
      continue;
    }
    if (r.op == IoOpcode::kRead) {
      stats_.read_ops.fetch_add(1, std::memory_order_relaxed);
      stats_.bytes_read.fetch_add(static_cast<uint64_t>(r.result), std::memory_order_relaxed);
    } else {
      stats_.write_ops.fetch_add(1, std::memory_order_relaxed);
      stats_.bytes_written.fetch_add(static_cast<uint64_t>(r.result), std::memory_order_relaxed);
    }
  }
  return first_error;
}

std::shared_ptr<IoBackend> NewPreadBackend() { return std::make_shared<PreadBackend>(); }

}  // namespace lsm
