// Thread-safe bump allocator backing a memtable. Memory is released only
// when the arena is destroyed (with its memtable).
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace lsm {

class ConcurrentArena {
 public:
  static constexpr size_t kBlockSize = 1 << 20;
  static constexpr size_t kAlign = alignof(std::max_align_t);

  ConcurrentArena() { current_.store(NewBlockLocked(kBlockSize), std::memory_order_release); }
  ConcurrentArena(const ConcurrentArena&) = delete;
  ConcurrentArena& operator=(const ConcurrentArena&) = delete;

  // Lock-free in the common case: one fetch_add on the current block.
  char* Allocate(size_t bytes) {
    bytes = (bytes + kAlign - 1) & ~(kAlign - 1);
    allocated_.fetch_add(bytes, std::memory_order_relaxed);
    if (bytes > kBlockSize / 4) {
      std::lock_guard lk(mu_);
      return NewBlockLocked(bytes)->base;  // Oversized: private block, current untouched.
    }
    for (;;) {
      Block* b = current_.load(std::memory_order_acquire);
      const size_t off = b->used.fetch_add(bytes, std::memory_order_relaxed);
      if (off + bytes <= b->size) return b->base + off;
      std::lock_guard lk(mu_);
      if (current_.load(std::memory_order_relaxed) == b) {
        current_.store(NewBlockLocked(kBlockSize), std::memory_order_release);
      }
    }
  }

  // Bytes handed out. This, not the reserved total, drives memtable
  // switching: the first 1 MiB block would otherwise make any
  // write_buffer_size below 1 MiB switch on every write.
  [[nodiscard]] size_t MemoryUsage() const noexcept { return allocated_.load(std::memory_order_relaxed); }
  // Bytes reserved from the system (allocated blocks).
  [[nodiscard]] size_t ReservedBytes() const noexcept { return usage_.load(std::memory_order_relaxed); }

 private:
  struct Block {
    char* base;
    size_t size;
    std::atomic<size_t> used{0};
  };

  Block* NewBlockLocked(size_t size) {
    // operator new[] already guarantees __STDCPP_DEFAULT_NEW_ALIGNMENT__ >= kAlign.
    static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= kAlign);
    auto mem = std::make_unique_for_overwrite<char[]>(size);
    auto blk = std::make_unique<Block>();
    blk->base = mem.get();
    blk->size = size;
    Block* raw = blk.get();
    storage_.push_back(std::move(mem));
    blocks_.push_back(std::move(blk));
    usage_.fetch_add(size + sizeof(Block), std::memory_order_relaxed);
    return raw;
  }

  std::mutex mu_;
  std::atomic<Block*> current_{nullptr};
  std::vector<std::unique_ptr<char[]>> storage_;
  std::vector<std::unique_ptr<Block>> blocks_;
  std::atomic<size_t> usage_{0};
  std::atomic<size_t> allocated_{0};
};

}  // namespace lsm
