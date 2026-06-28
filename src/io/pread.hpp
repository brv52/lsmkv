// Synchronous fallback backend built on preadv2(2)/pwritev2(2).
#pragma once

#include <atomic>

#include "lsm/iobackend.hpp"

namespace lsm {

class PreadBackend final : public IoBackend {
 public:
  PreadBackend() = default;

  [[nodiscard]] std::string Name() const override { return "pread"; }
  Status Submit(std::span<IoRequest> reqs) override;

 private:
  // Transfers one request fully (or to EOF). Returns 0 or -errno.
  int64_t Execute(IoRequest& r);
  ssize_t TransferOnce(IoOpcode op, int fd, char* buf, size_t len, uint64_t off);

  // RWF_NOWAIT is tried first so that reads served from cache (buffered
  // fallback files) skip the blocking path. Writes that extend an O_DIRECT
  // file almost always need block allocation and return EAGAIN under NOWAIT,
  // so the first such EAGAIN turns the write fast path off.
  std::atomic<bool> read_nowait_{true};
  std::atomic<bool> write_nowait_{true};
  std::atomic<bool> have_v2_{true};  // preadv2 unavailable -> ENOSYS -> pread.
};

}  // namespace lsm
