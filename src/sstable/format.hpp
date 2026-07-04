// SSTable on-disk format.
//
//   [data block 0][pad] [data block 1][pad] ... [index block][pad] [bloom block][pad][footer]
//
// * Every block starts on a 4 KiB boundary so it can be fetched with one
//   aligned O_DIRECT read straight into a block-cache buffer. Data blocks
//   target Options::block_size (4 KiB) including the trailer, so a typical
//   block is exactly one page.
// * A block is `contents | type (1B, 0 = uncompressed) | masked crc32c (4B)`;
//   the CRC covers contents and type.
// * Index block: one entry per data block, key = last internal key in the
//   block, value = BlockHandle(offset, size of contents).
// * The 48-byte footer occupies the last 48 bytes of the file, and the file
//   length is always a multiple of 4 KiB:
//     fixed64 index_offset | fixed64 index_size | fixed64 bloom_offset |
//     fixed64 bloom_size | fixed32 format_version | fixed32 masked crc32c of
//     the preceding 36 bytes | fixed64 magic
#pragma once

#include <cstdint>
#include <string>

#include "lsm/slice.hpp"
#include "lsm/status.hpp"
#include "util/coding.hpp"

namespace lsm::sst {

inline constexpr uint64_t kTableMagic = 0x31764b4d534c7462ull;  // "btLSMKv1"
inline constexpr uint32_t kFormatVersion = 1;
inline constexpr size_t kFooterSize = 48;
inline constexpr size_t kBlockTrailerSize = 5;
inline constexpr uint8_t kNoCompression = 0;

struct BlockHandle {
  uint64_t offset = 0;
  uint64_t size = 0;  // Contents only, excluding the trailer.

  static constexpr size_t kMaxEncodedLength = 20;

  void EncodeTo(std::string* dst) const {
    PutVarint64(dst, offset);
    PutVarint64(dst, size);
  }
  bool DecodeFrom(Slice* input) { return GetVarint64(input, &offset) && GetVarint64(input, &size); }
};

struct Footer {
  BlockHandle index;
  BlockHandle bloom;

  void EncodeTo(char* dst) const;  // Writes exactly kFooterSize bytes.
  Status DecodeFrom(const char* src);
};

}  // namespace lsm::sst
