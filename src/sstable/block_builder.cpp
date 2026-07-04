#include "sstable/block_builder.hpp"

#include <algorithm>
#include <cassert>

#include "sstable/block.hpp"
#include "sstable/format.hpp"
#include "util/coding.hpp"
#include "util/crc32c.hpp"
#include "util/dbformat.hpp"

namespace lsm::sst {

// ---------------------------------------------------------------------------
// Footer

void Footer::EncodeTo(char* dst) const {
  EncodeFixed64(dst, index.offset);
  EncodeFixed64(dst + 8, index.size);
  EncodeFixed64(dst + 16, bloom.offset);
  EncodeFixed64(dst + 24, bloom.size);
  EncodeFixed32(dst + 32, kFormatVersion);
  EncodeFixed32(dst + 36, crc32c::Mask(crc32c::Value(dst, 36)));
  EncodeFixed64(dst + 40, kTableMagic);
}

Status Footer::DecodeFrom(const char* src) {
  if (DecodeFixed64(src + 40) != kTableMagic) return Status::Corruption("not an sstable (bad magic)");
  if (crc32c::Unmask(DecodeFixed32(src + 36)) != crc32c::Value(src, 36)) {
    return Status::Corruption("sstable footer checksum mismatch");
  }
  if (DecodeFixed32(src + 32) != kFormatVersion) return Status::NotSupported("unknown sstable format version");
  index.offset = DecodeFixed64(src);
  index.size = DecodeFixed64(src + 8);
  bloom.offset = DecodeFixed64(src + 16);
  bloom.size = DecodeFixed64(src + 24);
  return Status::OK();
}

// ---------------------------------------------------------------------------
// BlockBuilder

BlockBuilder::BlockBuilder(int restart_interval) : restart_interval_(std::max(1, restart_interval)) {
  restarts_.push_back(0);
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
  assert(!finished_);
  size_t shared = 0;
  if (counter_ < restart_interval_) {
    const size_t min_len = std::min(last_key_.size(), key.size());
    while (shared < min_len && last_key_[shared] == key[shared]) ++shared;
  } else {
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;
  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());
  last_key_.resize(shared);
  last_key_.append(key.data() + shared, non_shared);
  ++counter_;
}

Slice BlockBuilder::Finish() {
  for (uint32_t r : restarts_) PutFixed32(&buffer_, r);
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return {buffer_.data(), buffer_.size()};
}

// ---------------------------------------------------------------------------
// Block

Block::Block(AlignedBuffer buf, size_t contents_size) : buf_(std::move(buf)), size_(contents_size) {
  if (size_ < 4 || size_ > buf_.capacity()) return;
  num_restarts_ = DecodeFixed32(buf_.data() + size_ - 4);
  const uint64_t max_restarts = (size_ - 4) / 4;
  if (num_restarts_ == 0 || num_restarts_ > max_restarts) return;
  restart_offset_ = static_cast<uint32_t>(size_ - 4 - 4ull * num_restarts_);
  ok_ = true;
}

class BlockIter final : public Iterator {
 public:
  BlockIter(const Block* b, Block::KeyOrder order)
      : data_(b->buf_.data()),
        restarts_(b->restart_offset_),
        num_restarts_(b->num_restarts_),
        order_(order),
        current_(restarts_),
        restart_index_(num_restarts_) {}

  [[nodiscard]] bool Valid() const override { return current_ < restarts_; }
  [[nodiscard]] Status status() const override { return status_; }
  [[nodiscard]] Slice key() const override { return {key_.data(), key_.size()}; }
  [[nodiscard]] Slice value() const override { return value_; }

  void Next() override {
    assert(Valid());
    ParseNextKey();
  }

  void SeekToFirst() override {
    SeekToRestartPoint(0);
    ParseNextKey();
  }

  void Seek(const Slice& target) override {
    uint32_t left = 0, right = num_restarts_ - 1;
    while (left < right) {
      const uint32_t mid = (left + right + 1) / 2;
      const uint32_t off = RestartPoint(mid);
      uint32_t shared, non_shared, value_len;
      const char* p = DecodeEntry(data_ + off, data_ + restarts_, &shared, &non_shared, &value_len);
      if (p == nullptr || shared != 0) {
        Corrupt();
        return;
      }
      if (Compare(Slice(p, non_shared), target) < 0) left = mid;
      else right = mid - 1;
    }
    SeekToRestartPoint(left);
    for (;;) {
      if (!ParseNextKey()) return;
      if (Compare(key(), target) >= 0) return;
    }
  }

 private:
  int Compare(const Slice& a, const Slice& b) const noexcept {
    return order_ == Block::KeyOrder::kInternal ? InternalKeyComparator::Compare(a, b) : a.compare(b);
  }

  uint32_t RestartPoint(uint32_t i) const noexcept { return DecodeFixed32(data_ + restarts_ + i * 4); }

  void SeekToRestartPoint(uint32_t i) {
    key_.clear();
    restart_index_ = i;
    const uint32_t off = RestartPoint(i);
    value_ = Slice(data_ + off, 0);  // NextEntryOffset() == off.
  }

  uint32_t NextEntryOffset() const noexcept {
    return static_cast<uint32_t>((value_.data() + value_.size()) - data_);
  }

  static const char* DecodeEntry(const char* p, const char* limit, uint32_t* shared, uint32_t* non_shared,
                                 uint32_t* value_len) {
    if (limit - p < 3) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, non_shared)) == nullptr) return nullptr;
    if ((p = GetVarint32Ptr(p, limit, value_len)) == nullptr) return nullptr;
    if (static_cast<uint64_t>(limit - p) < uint64_t{*non_shared} + *value_len) return nullptr;
    return p;
  }

  bool ParseNextKey() {
    current_ = NextEntryOffset();
    const char* p = data_ + current_;
    const char* limit = data_ + restarts_;
    if (p >= limit) {
      current_ = restarts_;
      restart_index_ = num_restarts_;
      return false;
    }
    uint32_t shared, non_shared, value_len;
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_len);
    if (p == nullptr || key_.size() < shared) {
      Corrupt();
      return false;
    }
    key_.resize(shared);
    key_.append(p, non_shared);
    value_ = Slice(p + non_shared, value_len);
    while (restart_index_ + 1 < num_restarts_ && RestartPoint(restart_index_ + 1) < current_) ++restart_index_;
    return true;
  }

  void Corrupt() {
    current_ = restarts_;
    restart_index_ = num_restarts_;
    status_ = Status::Corruption("bad entry in block");
    key_.clear();
    value_ = Slice();
  }

  const char* const data_;
  const uint32_t restarts_;
  const uint32_t num_restarts_;
  const Block::KeyOrder order_;
  uint32_t current_;
  uint32_t restart_index_;
  std::string key_;
  Slice value_;
  Status status_;
};

std::unique_ptr<Iterator> Block::NewIterator(KeyOrder order) const {
  if (!ok_) {
    struct ErrorIter final : Iterator {
      [[nodiscard]] bool Valid() const override { return false; }
      void SeekToFirst() override {}
      void Seek(const Slice&) override {}
      void Next() override {}
      [[nodiscard]] Slice key() const override { return {}; }
      [[nodiscard]] Slice value() const override { return {}; }
      [[nodiscard]] Status status() const override { return Status::Corruption("malformed block"); }
    };
    return std::make_unique<ErrorIter>();
  }
  return std::make_unique<BlockIter>(this, order);
}

}  // namespace lsm::sst
