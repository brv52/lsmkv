// Immutable, parsed block plus its iterator.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "lsm/iobackend.hpp"
#include "lsm/iterator.hpp"

namespace lsm::sst {

// Owns the aligned buffer the block was read into (zero-copy: the DMA target
// is the cached object) and views its contents.
class Block {
 public:
  // `contents_size` excludes trailer and padding.
  Block(AlignedBuffer buf, size_t contents_size);
  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  [[nodiscard]] bool valid() const noexcept { return ok_; }
  [[nodiscard]] size_t charge() const noexcept { return buf_.capacity() + sizeof(*this); }
  [[nodiscard]] Slice contents() const noexcept { return {buf_.data(), size_}; }

  // Iterates entries; `cmp` is the key comparator (internal or bytewise).
  // The block must outlive the iterator (hold the shared_ptr).
  enum class KeyOrder { kInternal, kBytewise };
  [[nodiscard]] std::unique_ptr<Iterator> NewIterator(KeyOrder order) const;

 private:
  friend class BlockIter;
  AlignedBuffer buf_;
  size_t size_;
  uint32_t restart_offset_ = 0;
  uint32_t num_restarts_ = 0;
  bool ok_ = false;
};

}  // namespace lsm::sst
