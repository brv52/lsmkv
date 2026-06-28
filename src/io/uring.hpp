// io_uring backend: sharded submission rings, registered (fixed) buffers and
// a sparse fixed-file table.
#pragma once

#include <liburing.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "lsm/iobackend.hpp"

namespace lsm {

// A slab of equally sized aligned buffers that is registered with every ring
// (io_uring_register_buffers), so a buffer index is valid on any ring.
class FixedBufferPool final : public BufferRecycler {
 public:
  FixedBufferPool(size_t count, size_t buf_size);
  ~FixedBufferPool() override;

  // Returns a lease, or an empty buffer when exhausted / too small.
  AlignedBuffer Acquire(size_t capacity, const std::shared_ptr<FixedBufferPool>& self);
  void Recycle(char* data, int index) noexcept override;

  [[nodiscard]] const std::vector<iovec>& iovecs() const noexcept { return iovecs_; }
  [[nodiscard]] size_t buffer_size() const noexcept { return buf_size_; }

 private:
  char* slab_ = nullptr;
  size_t buf_size_;
  std::vector<iovec> iovecs_;
  std::mutex mu_;
  std::vector<int> free_;
};

class UringBackend final : public IoBackend {
 public:
  // Use Create(); the constructor leaves the backend unusable on failure.
  static std::shared_ptr<UringBackend> Create(const Options& options, Status* status);
  ~UringBackend() override;

  [[nodiscard]] std::string Name() const override;
  void RegisterFile(int fd) override;
  void UnregisterFile(int fd) override;
  Status Submit(std::span<IoRequest> reqs) override;
  AlignedBuffer AllocateBuffer(size_t capacity) override;

  [[nodiscard]] bool fixed_buffers_enabled() const noexcept { return pool_ != nullptr; }
  [[nodiscard]] bool fixed_files_enabled() const noexcept { return fixed_files_; }

 private:
  struct Ring {
    std::mutex mu;
    io_uring ring{};
    bool initialized = false;
  };

  explicit UringBackend(const Options& options);
  Status Init();
  void SetupFixedBuffers();
  void SetupFixedFiles();
  Ring& PickRing() noexcept;
  // Returns the fixed-file slot for fd or -1.
  int SlotFor(int fd) const;
  Status SubmitOnRing(Ring& r, std::span<IoRequest> reqs);

  const unsigned queue_depth_;
  const unsigned num_rings_;
  const unsigned fixed_buffer_count_;
  const size_t fixed_buffer_size_;
  const bool want_fixed_files_;
  const bool sqpoll_;

  std::vector<std::unique_ptr<Ring>> rings_;
  bool use_rw_ops_ = true;  // IORING_OP_READ/WRITE (5.6+); else READV/WRITEV.
  std::shared_ptr<FixedBufferPool> pool_;

  bool fixed_files_ = false;
  unsigned file_table_size_ = 0;
  mutable std::shared_mutex files_mu_;
  std::unordered_map<int, int> fd_to_slot_;
  std::vector<int> free_slots_;
};

}  // namespace lsm
