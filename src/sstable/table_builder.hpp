// Streams sorted internal keys into an SSTable file with aligned direct I/O.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "lsm/iobackend.hpp"
#include "lsm/options.hpp"
#include "lsm/status.hpp"
#include "sstable/block_builder.hpp"
#include "sstable/bloom.hpp"
#include "sstable/format.hpp"

namespace lsm::sst {

class TableBuilder {
 public:
  // Creates `path` exclusively (O_DIRECT per options).
  static Status Create(const Options& options, IoBackend* io, const std::string& path,
                       std::unique_ptr<TableBuilder>* out);
  ~TableBuilder();
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // REQUIRES: ikey > every previously added key (internal key order).
  void Add(const Slice& ikey, const Slice& value);
  // Writes index, bloom filter and footer, fdatasyncs and closes the file.
  // The caller still has to fsync the directory before referencing the file.
  Status Finish();
  // Closes without finishing; the caller deletes the file.
  void Abandon();

  [[nodiscard]] Status status() const { return status_; }
  [[nodiscard]] uint64_t NumEntries() const noexcept { return num_entries_; }
  // Bytes of the file so far (buffered + written), padding included.
  [[nodiscard]] uint64_t FileSize() const noexcept { return written_ + out_len_; }
  [[nodiscard]] const std::string& smallest_key() const noexcept { return smallest_; }
  [[nodiscard]] const std::string& largest_key() const noexcept { return largest_; }
  [[nodiscard]] uint64_t bytes_written() const noexcept { return written_; }

 private:
  TableBuilder(const Options& options, IoBackend* io, int fd);

  void FlushDataBlock();
  // Appends contents + trailer; returns its handle. Pads to 4 KiB if `pad`.
  BlockHandle AppendBlock(const Slice& contents, bool pad);
  void EnsureRoom(size_t n);
  void FlushOut();
  void CloseFd();

  const Options options_;
  IoBackend* const io_;
  int fd_;
  AlignedBuffer out_;
  size_t out_len_ = 0;
  uint64_t written_ = 0;  // File offset of out_[0].

  BlockBuilder data_block_;
  BlockBuilder index_block_;
  BloomFilterBuilder bloom_;
  std::string last_user_key_;
  std::string smallest_;
  std::string largest_;
  uint64_t num_entries_ = 0;
  Status status_;
  bool closed_ = false;
};

}  // namespace lsm::sst
