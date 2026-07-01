// Sequential WAL reader used by recovery.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "lsm/db.hpp"
#include "lsm/iobackend.hpp"
#include "wal/log_format.hpp"

namespace lsm::wal {

// Reads whole batches in log order. Recovery semantics are point-in-time:
// the first malformed record (bad checksum, truncated payload, invalid type,
// incomplete batch) ends the log. Anything after it was never acknowledged
// as part of a contiguous durable prefix, so it is dropped and reported via
// dropped_bytes()/tail_reason().
class LogReader {
 public:
  static Status Open(IoBackend* io, const std::string& path, const Options& options,
                     std::unique_ptr<LogReader>* out);
  ~LogReader();
  LogReader(const LogReader&) = delete;
  LogReader& operator=(const LogReader&) = delete;

  // Reads the next complete batch. Sets *eof when the log ends (cleanly or
  // at a corrupt tail); returns non-OK only for I/O errors.
  Status ReadBatch(WriteBatch* batch, uint64_t* first_seq, bool* eof);

  // Offset just past the last fully decoded batch.
  [[nodiscard]] uint64_t good_offset() const noexcept { return good_offset_; }
  // Bytes between good_offset() and end of file (includes preallocated
  // space) when the log ended at a malformed record; 0 for a clean end.
  [[nodiscard]] uint64_t dropped_bytes() const noexcept { return dropped_bytes_; }
  [[nodiscard]] const std::string& tail_reason() const noexcept { return tail_reason_; }

 private:
  enum class Result { kRecord, kEof, kBad };

  LogReader(IoBackend* io, int fd, uint64_t file_size);
  // Makes [pos_, pos_+n) resident. Returns false if the file is too short.
  Status Ensure(size_t n, bool* ok);
  Status ReadRecord(RecordType* type, Slice* key, Slice* value, Result* result);
  void MarkTail(const std::string& reason);

  IoBackend* const io_;
  int fd_;
  const uint64_t file_size_;
  AlignedBuffer buf_;
  uint64_t buf_off_ = 0;  // File offset of buf_[0] (aligned).
  size_t buf_len_ = 0;
  uint64_t pos_ = 0;
  uint64_t good_offset_ = 0;
  uint64_t dropped_bytes_ = 0;
  std::string tail_reason_;
  bool done_ = false;
};

}  // namespace lsm::wal
