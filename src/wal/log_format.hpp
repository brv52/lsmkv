// WAL on-disk format.
//
//   record := CRC32C (4B) | Payload Length (4B) | Type (1B) | Key Length (2B) | Key | Value
//
// * CRC32C is masked (crc32c::Mask) and covers every byte after itself:
//   length, type, key length, key and value.
// * Payload Length = key length + value length.
// * The file is a sequence of 4 KiB blocks written with O_DIRECT. A record
//   never starts in the last kHeaderSize-1 bytes of a block: those bytes are
//   zero filler and the reader skips to the next block boundary. Records may
//   otherwise span blocks.
// * A zero header (length == 0 && type == 0) marks the logical end of the log
//   (unwritten / preallocated space).
// * Every WriteBatch is framed by a kBatchHeader record whose value is
//   fixed64(first sequence) | fixed32(op count), followed by exactly `count`
//   kPut/kDelete records. Recovery applies a batch only if every record of it
//   is intact, which keeps batches atomic across torn writes.
#pragma once

#include <cstddef>
#include <cstdint>

namespace lsm::wal {

inline constexpr size_t kBlockSize = 4096;
inline constexpr size_t kHeaderSize = 4 + 4 + 1 + 2;
inline constexpr size_t kMaxKeySize = 0xffff;
inline constexpr size_t kBatchHeaderValueSize = 8 + 4;

enum class RecordType : uint8_t {
  kZero = 0,  // Never written; reserved for "unwritten space".
  kPut = 1,
  kDelete = 2,
  kPadding = 3,  // Payload is ignored; used by Options::wal_pad_commits.
  kBatchHeader = 4,
};

inline constexpr uint8_t kMaxRecordType = static_cast<uint8_t>(RecordType::kBatchHeader);

}  // namespace lsm::wal
