// Leveled compaction: picking, k-way merge, and output generation.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "lsm/iterator.hpp"
#include "manifest/version_set.hpp"
#include "sstable/table_reader.hpp"

namespace lsm {

// Min-heap merge of N sorted internal-key iterators. Entries with equal keys
// cannot occur (sequence numbers are unique), so ties need no rule.
std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children);

struct Compaction {
  int level = 0;         // Inputs come from `level` and `level + 1`.
  int output_level = 1;
  std::vector<Version::FilePtr> inputs[2];
  std::shared_ptr<const Version> input_version;
  uint64_t max_output_file_size = 0;
  double score = 0;

  // A single file with nothing beneath it to merge can be re-linked to the
  // next level without being rewritten.
  [[nodiscard]] bool IsTrivialMove() const {
    return inputs[0].size() == 1 && inputs[1].empty();
  }

  // True if no level below output_level can contain user_key, so a tombstone
  // for it is no longer needed. Calls must use ascending keys.
  bool IsBaseLevelForKey(const Slice& user_key);

  void AddInputDeletions(VersionEdit* edit) const;
  [[nodiscard]] uint64_t InputBytes() const;

 private:
  std::vector<size_t> level_ptrs_;
};

// Chooses the level with the highest score >= 1:
//   L0: file count / level0_compaction_trigger
//   Li: level bytes / (max_bytes_for_level_base * T^(i-1))
// Returns nullptr when nothing needs compaction. `force_level` (>=0) compacts
// that level regardless of score (used by CompactAll).
std::unique_ptr<Compaction> PickCompaction(const VersionSet& vset, int force_level = -1);

struct CompactionResult {
  std::vector<FileMetaData> outputs;
  uint64_t bytes_read = 0;
  uint64_t bytes_written = 0;
  uint64_t entries_in = 0;
  uint64_t entries_dropped = 0;
};

// Merges the compaction's inputs into new tables. `new_file_number`
// allocates output numbers (the caller tracks them as pending outputs until
// the edit is installed). Output tables are fully synced; the directory is
// not.
Status RunCompaction(Compaction* c, const Options& options, const std::string& dbname, IoBackend* io,
                     sst::TableCache* tables, const std::function<uint64_t()>& new_file_number,
                     const std::function<bool()>& should_stop, CompactionResult* result);

// Writes every entry of `it` (internal keys, ascending) into one new table.
// Used for memtable flushes. Leaves meta->file_size == 0 if `it` is empty.
Status BuildTable(const Options& options, const std::string& dbname, IoBackend* io, Iterator* it,
                  uint64_t file_number, FileMetaData* meta);

}  // namespace lsm
