#include "wal/log_reader.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "util/coding.hpp"
#include "util/crc32c.hpp"
#include "util/file_util.hpp"

namespace lsm::wal {

namespace {
constexpr size_t kReadChunk = 1 << 20;
}

LogReader::LogReader(IoBackend* io, int fd, uint64_t file_size)
    : io_(io), fd_(fd), file_size_(file_size), buf_(AlignedBuffer::Allocate(kReadChunk)) {}

LogReader::~LogReader() {
  io_->UnregisterFile(fd_);
  ::close(fd_);
}

Status LogReader::Open(IoBackend* io, const std::string& path, const Options& options,
                       std::unique_ptr<LogReader>* out) {
  int fd = -1;
  LSM_RETURN_IF_ERROR(
      OpenFile(path, O_RDONLY, 0, options.use_direct_io, options.allow_buffered_fallback, &fd, nullptr));
  uint64_t size = 0;
  Status s = GetFileSize(fd, &size);
  if (!s.ok()) {
    ::close(fd);
    return s;
  }
  io->RegisterFile(fd);
  out->reset(new LogReader(io, fd, size));
  return Status::OK();
}

Status LogReader::Ensure(size_t n, bool* ok) {
  *ok = false;
  if (pos_ + n <= buf_off_ + buf_len_ && pos_ >= buf_off_) {
    *ok = true;
    return Status::OK();
  }
  if (pos_ + n > file_size_) return Status::OK();

  // Slide the window so it starts at the block containing pos_.
  const uint64_t new_off = AlignDown(pos_);
  const uint64_t buf_end = buf_off_ + buf_len_;
  size_t keep = 0;
  if (new_off >= buf_off_ && new_off < buf_end) {
    keep = static_cast<size_t>(buf_end - new_off);
    std::memmove(buf_.data(), buf_.data() + (new_off - buf_off_), keep);
  }
  const size_t need = static_cast<size_t>(pos_ + n - new_off);
  if (need > buf_.capacity()) {
    AlignedBuffer bigger = AlignedBuffer::Allocate(AlignUp(need) + kReadChunk);
    std::memcpy(bigger.data(), buf_.data(), keep);
    buf_ = std::move(bigger);
  }
  buf_off_ = new_off;
  buf_len_ = keep;

  while (buf_len_ < need) {
    const uint64_t read_off = buf_off_ + buf_len_;  // Aligned: only the final read can be short.
    const size_t want = static_cast<size_t>(
        std::min<uint64_t>(buf_.capacity() - buf_len_, AlignUp(file_size_ - read_off)));
    if (want == 0) return Status::OK();
    size_t got = 0;
    LSM_RETURN_IF_ERROR(io_->Read(fd_, buf_.data() + buf_len_, want, read_off, &got));
    if (got == 0) return Status::OK();
    buf_len_ += got;
    if (!IsAligned(got)) break;
  }
  *ok = buf_len_ >= need;
  return Status::OK();
}

Status LogReader::ReadRecord(RecordType* type, Slice* key, Slice* value, Result* result) {
  const size_t in_block = pos_ % kBlockSize;
  if (kBlockSize - in_block < kHeaderSize) pos_ += kBlockSize - in_block;  // Block-tail filler.

  bool ok;
  LSM_RETURN_IF_ERROR(Ensure(kHeaderSize, &ok));
  if (!ok) {
    *result = Result::kEof;
    return Status::OK();
  }
  const char* h = buf_.data() + (pos_ - buf_off_);
  const uint32_t masked_crc = DecodeFixed32(h);
  const uint32_t len = DecodeFixed32(h + 4);
  const auto t = static_cast<uint8_t>(h[8]);
  const uint16_t klen = DecodeFixed16(h + 9);

  if (len == 0 && t == 0) {
    *result = Result::kEof;  // Unwritten (zeroed / preallocated) space.
    return Status::OK();
  }
  if (t == 0 || t > kMaxRecordType || klen > len) {
    MarkTail("invalid record header");
    *result = Result::kBad;
    return Status::OK();
  }
  LSM_RETURN_IF_ERROR(Ensure(kHeaderSize + len, &ok));
  if (!ok) {
    MarkTail("truncated record");
    *result = Result::kBad;
    return Status::OK();
  }
  h = buf_.data() + (pos_ - buf_off_);  // Ensure may have moved the window.
  const uint32_t actual = crc32c::Value(h + 4, kHeaderSize - 4 + len);
  if (crc32c::Unmask(masked_crc) != actual) {
    MarkTail("checksum mismatch");
    *result = Result::kBad;
    return Status::OK();
  }
  *type = static_cast<RecordType>(t);
  *key = Slice(h + kHeaderSize, klen);
  *value = Slice(h + kHeaderSize + klen, len - klen);
  pos_ += kHeaderSize + len;
  *result = Result::kRecord;
  return Status::OK();
}

void LogReader::MarkTail(const std::string& reason) {
  tail_reason_ = reason;
  dropped_bytes_ = file_size_ > good_offset_ ? file_size_ - good_offset_ : 0;
}

Status LogReader::ReadBatch(WriteBatch* batch, uint64_t* first_seq, bool* eof) {
  batch->Clear();
  *eof = false;
  if (done_) {
    *eof = true;
    return Status::OK();
  }
  bool in_batch = false;
  uint32_t expected = 0;
  for (;;) {
    RecordType type{};
    Slice key, value;
    Result r;
    LSM_RETURN_IF_ERROR(ReadRecord(&type, &key, &value, &r));
    if (r == Result::kEof) {
      if (in_batch) MarkTail("incomplete batch at end of log");
      done_ = *eof = true;
      batch->Clear();
      return Status::OK();
    }
    if (r == Result::kBad) {
      done_ = *eof = true;
      batch->Clear();
      return Status::OK();
    }
    switch (type) {
      case RecordType::kPadding:
        if (in_batch) {
          MarkTail("padding inside batch");
          done_ = *eof = true;
          batch->Clear();
          return Status::OK();
        }
        good_offset_ = pos_;
        continue;
      case RecordType::kBatchHeader:
        if (in_batch || value.size() != kBatchHeaderValueSize || !key.empty()) {
          MarkTail("unexpected batch header");
          done_ = *eof = true;
          batch->Clear();
          return Status::OK();
        }
        *first_seq = DecodeFixed64(value.data());
        expected = DecodeFixed32(value.data() + 8);
        in_batch = true;
        if (expected == 0) {
          good_offset_ = pos_;
          return Status::OK();
        }
        continue;
      case RecordType::kPut:
      case RecordType::kDelete:
        if (!in_batch) {
          MarkTail("operation outside batch");
          done_ = *eof = true;
          batch->Clear();
          return Status::OK();
        }
        if (type == RecordType::kPut) batch->Put(key, value);
        else batch->Delete(key);
        if (batch->Count() == expected) {
          good_offset_ = pos_;
          return Status::OK();
        }
        continue;
      case RecordType::kZero:
        break;
    }
    MarkTail("invalid record type");
    done_ = *eof = true;
    batch->Clear();
    return Status::OK();
  }
}

}  // namespace lsm::wal
