#include "sstable/table_reader.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "sstable/bloom.hpp"
#include "util/crc32c.hpp"
#include "util/dbformat.hpp"
#include "util/file_util.hpp"

namespace lsm::sst {

TableReader::TableReader(IoBackend* io, LruCache* cache, TableStats* stats, int fd, uint64_t file_number,
                         uint64_t size)
    : io_(io), cache_(cache), stats_(stats), fd_(fd), file_number_(file_number), file_size_(size) {}

TableReader::~TableReader() {
  io_->UnregisterFile(fd_);
  ::close(fd_);
}

Status TableReader::ReadBlockFromFile(IoBackend* io, int fd, uint64_t file_size, const BlockHandle& h,
                                      bool verify, std::shared_ptr<const Block>* out) {
  if (!IsAligned(h.offset) || h.offset + h.size + kBlockTrailerSize > file_size) {
    return Status::Corruption("block handle out of range");
  }
  const size_t len = AlignUp(h.size + kBlockTrailerSize);
  // The DMA target is the buffer that ends up in the cache: no copy.
  AlignedBuffer buf = AlignedBuffer::Allocate(len);
  size_t got = 0;
  LSM_RETURN_IF_ERROR(io->Read(fd, buf.data(), len, h.offset, &got));
  if (got < h.size + kBlockTrailerSize) return Status::Corruption("truncated block read");
  return ParseBlock(std::move(buf), h, verify, out);
}

Status TableReader::ParseBlock(AlignedBuffer buf, const BlockHandle& h, bool verify,
                               std::shared_ptr<const Block>* out) {
  const char* p = buf.data();
  if (verify) {
    const uint32_t expected = crc32c::Unmask(DecodeFixed32(p + h.size + 1));
    if (crc32c::Value(p, h.size + 1) != expected) return Status::Corruption("block checksum mismatch");
  }
  if (static_cast<uint8_t>(p[h.size]) != kNoCompression) return Status::NotSupported("compressed block");
  *out = std::make_shared<const Block>(std::move(buf), static_cast<size_t>(h.size));
  return Status::OK();
}

Status TableReader::Open(const Options& options, IoBackend* io, LruCache* cache, TableStats* stats,
                         uint64_t file_number, const std::string& path, std::unique_ptr<TableReader>* out) {
  int fd = -1;
  LSM_RETURN_IF_ERROR(
      OpenFile(path, O_RDONLY, 0, options.use_direct_io, options.allow_buffered_fallback, &fd, nullptr));
  ScopedFd guard(fd);
  uint64_t size = 0;
  LSM_RETURN_IF_ERROR(GetFileSize(fd, &size));
  if (size < kIoAlignment || !IsAligned(size)) return Status::Corruption("sstable size not page aligned: " + path);

  AlignedBuffer tail = AlignedBuffer::Allocate(kIoAlignment);
  size_t got = 0;
  LSM_RETURN_IF_ERROR(io->Read(fd, tail.data(), kIoAlignment, size - kIoAlignment, &got));
  if (got != kIoAlignment) return Status::Corruption("short footer read: " + path);
  Footer footer;
  LSM_RETURN_IF_ERROR(footer.DecodeFrom(tail.data() + kIoAlignment - kFooterSize));

  std::unique_ptr<TableReader> r(new TableReader(io, cache, stats, guard.release(), file_number, size));
  io->RegisterFile(r->fd_);
  LSM_RETURN_IF_ERROR(ReadBlockFromFile(io, r->fd_, size, footer.index, true, &r->index_));
  if (!r->index_->valid()) return Status::Corruption("malformed index block: " + path);
  LSM_RETURN_IF_ERROR(ReadBlockFromFile(io, r->fd_, size, footer.bloom, true, &r->bloom_));
  r->bloom_data_ = r->bloom_->contents();  // Not a restart-array block; only the bytes matter.
  *out = std::move(r);
  return Status::OK();
}

bool TableReader::BloomMayContain(const Slice& user_key) const noexcept {
  return BloomFilterBuilder::MayContain(bloom_data_, BloomHash(user_key));
}

Status TableReader::GetBlock(const ReadOptions& ro, const BlockHandle& handle,
                             std::shared_ptr<const Block>* out) const {
  const CacheKey key{file_number_, handle.offset};
  if (cache_ != nullptr) {
    if (auto v = cache_->Lookup(key)) {
      *out = std::static_pointer_cast<const Block>(v);
      return Status::OK();
    }
  }
  std::shared_ptr<const Block> b;
  LSM_RETURN_IF_ERROR(ReadBlockFromFile(io_, fd_, file_size_, handle, ro.verify_checksums, &b));
  if (stats_) stats_->blocks_read.fetch_add(1, std::memory_order_relaxed);
  if (!b->valid()) return Status::Corruption("malformed data block");
  if (cache_ != nullptr && ro.fill_cache) cache_->Insert(key, b, b->charge());
  *out = std::move(b);
  return Status::OK();
}

Status TableReader::GetBlock(const ReadOptions& ro, const BlockHandle& handle, ReadaheadBuffer* ra,
                             std::shared_ptr<const Block>* out) const {
  if (ra == nullptr || ro.readahead_size == 0) return GetBlock(ro, handle, out);
  const CacheKey key{file_number_, handle.offset};
  if (cache_ != nullptr) {
    if (auto v = cache_->Lookup(key)) {
      *out = std::static_pointer_cast<const Block>(v);
      return Status::OK();
    }
  }
  const uint64_t need = handle.size + kBlockTrailerSize;
  if (!IsAligned(handle.offset) || handle.offset + need > file_size_) {
    return Status::Corruption("block handle out of range");
  }
  if (handle.offset < ra->offset || handle.offset + need > ra->offset + ra->len) {
    // Refill: one large aligned read starting at this block.
    const size_t want = static_cast<size_t>(
        std::min<uint64_t>(AlignUp(std::max<uint64_t>(ro.readahead_size, need)), file_size_ - handle.offset));
    if (ra->buf.capacity() < want) ra->buf = io_->AllocateBuffer(want);
    size_t got = 0;
    LSM_RETURN_IF_ERROR(io_->Read(fd_, ra->buf.data(), want, handle.offset, &got, ra->buf.fixed_index()));
    ra->offset = handle.offset;
    ra->len = got;
    if (stats_) stats_->blocks_read.fetch_add(1, std::memory_order_relaxed);
    if (got < need) return Status::Corruption("truncated block read");
  }
  // Copy the block out of the window so it can outlive it (and be cached).
  AlignedBuffer b = AlignedBuffer::Allocate(need);
  std::memcpy(b.data(), ra->buf.data() + (handle.offset - ra->offset), need);
  std::shared_ptr<const Block> block;
  LSM_RETURN_IF_ERROR(ParseBlock(std::move(b), handle, ro.verify_checksums, &block));
  if (!block->valid()) return Status::Corruption("malformed data block");
  if (cache_ != nullptr && ro.fill_cache) cache_->Insert(key, block, block->charge());
  *out = std::move(block);
  return Status::OK();
}

Status TableReader::Get(const ReadOptions& ro, const Slice& lookup_ikey, bool* found, ValueType* type,
                        std::string* value) {
  *found = false;
  const Slice user_key = ExtractUserKey(lookup_ikey);
  if (stats_) stats_->bloom_checked.fetch_add(1, std::memory_order_relaxed);
  if (!BloomMayContain(user_key)) {
    if (stats_) stats_->bloom_useful.fetch_add(1, std::memory_order_relaxed);
    return Status::OK();
  }
  auto index_it = index_->NewIterator(Block::KeyOrder::kInternal);
  index_it->Seek(lookup_ikey);
  if (!index_it->Valid()) return index_it->status();

  Slice hv = index_it->value();
  BlockHandle h;
  if (!h.DecodeFrom(&hv)) return Status::Corruption("bad block handle in index");
  std::shared_ptr<const Block> block;
  LSM_RETURN_IF_ERROR(GetBlock(ro, h, &block));
  auto it = block->NewIterator(Block::KeyOrder::kInternal);
  it->Seek(lookup_ikey);
  if (!it->Valid()) return it->status();
  const Slice k = it->key();
  if (k.size() < 8 || ExtractUserKey(k) != user_key) return Status::OK();
  *found = true;
  *type = ExtractValueType(k);
  if (*type == ValueType::kValue) value->assign(it->value().data(), it->value().size());
  return Status::OK();
}

namespace {

// index block -> data block iterator.
class TwoLevelIterator final : public Iterator {
 public:
  TwoLevelIterator(const TableReader* table, std::unique_ptr<Iterator> index, const ReadOptions& ro)
      : table_(table), index_(std::move(index)), ro_(ro) {}

  [[nodiscard]] bool Valid() const override { return data_ != nullptr && data_->Valid(); }
  [[nodiscard]] Slice key() const override { return data_->key(); }
  [[nodiscard]] Slice value() const override { return data_->value(); }
  [[nodiscard]] Status status() const override {
    if (!status_.ok()) return status_;
    if (!index_->status().ok()) return index_->status();
    return data_ ? data_->status() : Status::OK();
  }

  void SeekToFirst() override {
    index_->SeekToFirst();
    InitDataBlock();
    if (data_) data_->SeekToFirst();
    SkipEmptyBlocks();
  }
  void Seek(const Slice& target) override {
    index_->Seek(target);
    InitDataBlock();
    if (data_) data_->Seek(target);
    SkipEmptyBlocks();
  }
  void Next() override {
    data_->Next();
    SkipEmptyBlocks();
  }

 private:
  void InitDataBlock() {
    data_.reset();
    block_.reset();
    if (!index_->Valid()) return;
    Slice hv = index_->value();
    BlockHandle h;
    if (!h.DecodeFrom(&hv)) {
      status_ = Status::Corruption("bad block handle in index");
      return;
    }
    Status s = table_->GetBlock(ro_, h, &readahead_, &block_);
    if (!s.ok()) {
      status_ = s;
      return;
    }
    data_ = block_->NewIterator(Block::KeyOrder::kInternal);
  }
  void SkipEmptyBlocks() {
    while (status_.ok() && (data_ == nullptr || !data_->Valid())) {
      if (data_ && !data_->status().ok()) return;
      if (!index_->Valid()) {
        data_.reset();
        return;
      }
      index_->Next();
      InitDataBlock();
      if (data_) data_->SeekToFirst();
    }
  }

  const TableReader* table_;
  std::unique_ptr<Iterator> index_;
  const ReadOptions ro_;
  ReadaheadBuffer readahead_;
  std::shared_ptr<const Block> block_;  // Pins the current data block.
  std::unique_ptr<Iterator> data_;
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> TableReader::NewIterator(const ReadOptions& ro) const {
  return std::make_unique<TwoLevelIterator>(this, index_->NewIterator(Block::KeyOrder::kInternal), ro);
}

// ---------------------------------------------------------------------------
// TableCache

TableCache::TableCache(const Options& options, std::string dbname, IoBackend* io, LruCache* block_cache,
                       TableStats* stats)
    : options_(options), dbname_(std::move(dbname)), io_(io), block_cache_(block_cache), stats_(stats) {}

Status TableCache::Get(uint64_t file_number, std::shared_ptr<TableReader>* out) {
  {
    std::lock_guard lk(mu_);
    if (auto it = tables_.find(file_number); it != tables_.end()) {
      *out = it->second;
      return Status::OK();
    }
  }
  std::unique_ptr<TableReader> r;
  LSM_RETURN_IF_ERROR(TableReader::Open(options_, io_, block_cache_, stats_, file_number,
                                        TableFileName(dbname_, file_number), &r));
  std::shared_ptr<TableReader> sp(std::move(r));
  std::lock_guard lk(mu_);
  auto [it, inserted] = tables_.emplace(file_number, sp);
  *out = it->second;  // A concurrent opener may have won; use its reader.
  return Status::OK();
}

void TableCache::Evict(uint64_t file_number) {
  std::shared_ptr<TableReader> victim;
  std::lock_guard lk(mu_);
  if (auto it = tables_.find(file_number); it != tables_.end()) {
    victim = std::move(it->second);
    tables_.erase(it);
  }
}

}  // namespace lsm::sst
