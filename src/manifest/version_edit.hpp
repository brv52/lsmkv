// Delta between two Versions, and the per-file metadata it carries.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lsm/slice.hpp"
#include "lsm/status.hpp"
#include "util/coding.hpp"

namespace lsm {

struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest;  // Internal keys.
  std::string largest;
};

class VersionEdit {
 public:
  enum Tag : uint32_t {
    kLogNumber = 1,
    kNextFileNumber = 2,
    kLastSequence = 3,
    kCompactPointer = 4,
    kDeletedFile = 5,
    kNewFile = 6,
  };

  void SetLogNumber(uint64_t n) { log_number_ = n; }
  void SetNextFile(uint64_t n) { next_file_number_ = n; }
  void SetLastSequence(uint64_t s) { last_sequence_ = s; }
  void SetCompactPointer(int level, std::string key) { compact_pointers_.emplace_back(level, std::move(key)); }
  void AddFile(int level, FileMetaData f) { new_files_.emplace_back(level, std::move(f)); }
  void DeleteFile(int level, uint64_t number) { deleted_files_.emplace_back(level, number); }

  [[nodiscard]] const std::optional<uint64_t>& log_number() const noexcept { return log_number_; }
  [[nodiscard]] const std::optional<uint64_t>& next_file_number() const noexcept { return next_file_number_; }
  [[nodiscard]] const std::optional<uint64_t>& last_sequence() const noexcept { return last_sequence_; }
  [[nodiscard]] const auto& compact_pointers() const noexcept { return compact_pointers_; }
  [[nodiscard]] const auto& new_files() const noexcept { return new_files_; }
  [[nodiscard]] const auto& deleted_files() const noexcept { return deleted_files_; }

  void EncodeTo(std::string* dst) const {
    if (log_number_) {
      PutVarint32(dst, kLogNumber);
      PutVarint64(dst, *log_number_);
    }
    if (next_file_number_) {
      PutVarint32(dst, kNextFileNumber);
      PutVarint64(dst, *next_file_number_);
    }
    if (last_sequence_) {
      PutVarint32(dst, kLastSequence);
      PutVarint64(dst, *last_sequence_);
    }
    for (const auto& [level, key] : compact_pointers_) {
      PutVarint32(dst, kCompactPointer);
      PutVarint32(dst, static_cast<uint32_t>(level));
      PutLengthPrefixedSlice(dst, key);
    }
    for (const auto& [level, number] : deleted_files_) {
      PutVarint32(dst, kDeletedFile);
      PutVarint32(dst, static_cast<uint32_t>(level));
      PutVarint64(dst, number);
    }
    for (const auto& [level, f] : new_files_) {
      PutVarint32(dst, kNewFile);
      PutVarint32(dst, static_cast<uint32_t>(level));
      PutVarint64(dst, f.number);
      PutVarint64(dst, f.file_size);
      PutLengthPrefixedSlice(dst, f.smallest);
      PutLengthPrefixedSlice(dst, f.largest);
    }
  }

  Status DecodeFrom(Slice input) {
    *this = VersionEdit();
    while (!input.empty()) {
      uint32_t tag, level;
      uint64_t v;
      Slice s1, s2;
      if (!GetVarint32(&input, &tag)) return Status::Corruption("manifest: bad tag");
      switch (tag) {
        case kLogNumber:
          if (!GetVarint64(&input, &v)) return Status::Corruption("manifest: log number");
          log_number_ = v;
          break;
        case kNextFileNumber:
          if (!GetVarint64(&input, &v)) return Status::Corruption("manifest: next file");
          next_file_number_ = v;
          break;
        case kLastSequence:
          if (!GetVarint64(&input, &v)) return Status::Corruption("manifest: last sequence");
          last_sequence_ = v;
          break;
        case kCompactPointer:
          if (!GetVarint32(&input, &level) || !GetLengthPrefixedSlice(&input, &s1)) {
            return Status::Corruption("manifest: compact pointer");
          }
          compact_pointers_.emplace_back(static_cast<int>(level), s1.ToString());
          break;
        case kDeletedFile:
          if (!GetVarint32(&input, &level) || !GetVarint64(&input, &v)) {
            return Status::Corruption("manifest: deleted file");
          }
          deleted_files_.emplace_back(static_cast<int>(level), v);
          break;
        case kNewFile: {
          FileMetaData f;
          if (!GetVarint32(&input, &level) || !GetVarint64(&input, &f.number) ||
              !GetVarint64(&input, &f.file_size) || !GetLengthPrefixedSlice(&input, &s1) ||
              !GetLengthPrefixedSlice(&input, &s2) || s1.size() < 8 || s2.size() < 8) {
            return Status::Corruption("manifest: new file");
          }
          f.smallest = s1.ToString();
          f.largest = s2.ToString();
          new_files_.emplace_back(static_cast<int>(level), std::move(f));
          break;
        }
        default:
          return Status::Corruption("manifest: unknown tag " + std::to_string(tag));
      }
    }
    return Status::OK();
  }

 private:
  std::optional<uint64_t> log_number_;
  std::optional<uint64_t> next_file_number_;
  std::optional<uint64_t> last_sequence_;
  std::vector<std::pair<int, std::string>> compact_pointers_;
  std::vector<std::pair<int, uint64_t>> deleted_files_;
  std::vector<std::pair<int, FileMetaData>> new_files_;
};

}  // namespace lsm
