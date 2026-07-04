#include "sstable/table_builder.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

#include "util/crc32c.hpp"
#include "util/dbformat.hpp"

namespace lsm::sst {

namespace {
constexpr size_t kStagingSize = 1 << 20;  // Flush to the device in 1 MiB aligned chunks.
}

TableBuilder::TableBuilder(const Options& options, IoBackend* io, int fd)
    : options_(options),
      io_(io),
      fd_(fd),
      out_(io->AllocateBuffer(kStagingSize)),
      data_block_(options.block_restart_interval),
      index_block_(1),
      bloom_(options.bloom_bits_per_key) {}

TableBuilder::~TableBuilder() { CloseFd(); }

Status TableBuilder::Create(const Options& options, IoBackend* io, const std::string& path,
                            std::unique_ptr<TableBuilder>* out) {
  int fd = -1;
  LSM_RETURN_IF_ERROR(OpenFile(path, O_WRONLY | O_CREAT | O_EXCL, 0644, options.use_direct_io,
                               options.allow_buffered_fallback, &fd, nullptr));
  io->RegisterFile(fd);
  out->reset(new TableBuilder(options, io, fd));
  return Status::OK();
}

void TableBuilder::CloseFd() {
  if (fd_ >= 0) {
    io_->UnregisterFile(fd_);
    ::close(fd_);
    fd_ = -1;
  }
}

void TableBuilder::Add(const Slice& ikey, const Slice& value) {
  if (!status_.ok() || closed_) return;
  // Keep data blocks within one page: cut before the entry would overflow.
  const size_t entry_estimate = ikey.size() + value.size() + 3 * 5 + 4;
  if (!data_block_.empty() &&
      data_block_.CurrentSizeEstimate() + entry_estimate + kBlockTrailerSize > options_.block_size) {
    FlushDataBlock();
  }
  if (num_entries_ == 0) smallest_.assign(ikey.data(), ikey.size());
  largest_.assign(ikey.data(), ikey.size());

  const Slice user_key = ExtractUserKey(ikey);
  if (num_entries_ == 0 || user_key != Slice(last_user_key_)) {
    bloom_.AddHash(BloomHash(user_key));
    last_user_key_.assign(user_key.data(), user_key.size());
  }
  data_block_.Add(ikey, value);
  ++num_entries_;
}

void TableBuilder::FlushDataBlock() {
  if (data_block_.empty()) return;
  const std::string last = data_block_.last_key();
  const BlockHandle h = AppendBlock(data_block_.Finish(), /*pad=*/true);
  data_block_.Reset();
  std::string enc;
  h.EncodeTo(&enc);
  index_block_.Add(last, enc);
  if (out_len_ >= kStagingSize) FlushOut();
}

void TableBuilder::EnsureRoom(size_t n) {
  if (out_len_ + n <= out_.capacity()) return;
  FlushOut();
  if (n > out_.capacity()) out_ = io_->AllocateBuffer(AlignUp(n));  // Oversized block.
}

BlockHandle TableBuilder::AppendBlock(const Slice& contents, bool pad) {
  const size_t raw = contents.size() + kBlockTrailerSize;
  EnsureRoom(AlignUp(raw) + kIoAlignment);
  BlockHandle h{written_ + out_len_, contents.size()};
  char* p = out_.data() + out_len_;
  std::memcpy(p, contents.data(), contents.size());
  p[contents.size()] = static_cast<char>(kNoCompression);
  const uint32_t crc = crc32c::Value(p, contents.size() + 1);
  EncodeFixed32(p + contents.size() + 1, crc32c::Mask(crc));
  out_len_ += raw;
  if (pad) {
    const size_t padded = AlignUp(out_len_);
    std::memset(out_.data() + out_len_, 0, padded - out_len_);
    out_len_ = padded;
  }
  return h;
}

void TableBuilder::FlushOut() {
  if (out_len_ == 0 || !status_.ok()) return;
  // Everything appended so far ends on a page boundary except during Finish,
  // which pads before its final flush.
  const size_t len = AlignUp(out_len_);
  std::memset(out_.data() + out_len_, 0, len - out_len_);
  status_ = io_->Write(fd_, out_.data(), len, written_, out_.fixed_index());
  if (status_.ok()) {
    written_ += len;
    out_len_ = 0;
  }
}

Status TableBuilder::Finish() {
  if (closed_) return Status::InvalidArgument("table already finished");
  closed_ = true;
  FlushDataBlock();
  Footer footer;
  footer.index = AppendBlock(index_block_.Finish(), /*pad=*/true);
  footer.bloom = AppendBlock(bloom_.Finish(), /*pad=*/false);

  // The footer takes the last 48 bytes of the final page.
  EnsureRoom(2 * kIoAlignment);
  size_t in_page = (written_ + out_len_) % kIoAlignment;
  size_t pad = in_page <= kIoAlignment - kFooterSize ? kIoAlignment - kFooterSize - in_page
                                                     : 2 * kIoAlignment - kFooterSize - in_page;
  std::memset(out_.data() + out_len_, 0, pad);
  out_len_ += pad;
  footer.EncodeTo(out_.data() + out_len_);
  out_len_ += kFooterSize;
  FlushOut();
  if (status_.ok()) status_ = io_->Sync(fd_, SyncMode::kData);
  CloseFd();
  return status_;
}

void TableBuilder::Abandon() {
  closed_ = true;
  CloseFd();
}

}  // namespace lsm::sst
