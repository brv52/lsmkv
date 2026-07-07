// Versions (immutable LSM shape snapshots) and the VersionSet that persists
// them through the MANIFEST.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "lsm/iterator.hpp"
#include "lsm/options.hpp"
#include "manifest/version_edit.hpp"
#include "sstable/table_reader.hpp"
#include "util/dbformat.hpp"

namespace lsm {

class Version {
 public:
  using FilePtr = std::shared_ptr<const FileMetaData>;

  explicit Version(int num_levels) : files_(static_cast<size_t>(num_levels)) {}

  [[nodiscard]] int num_levels() const noexcept { return static_cast<int>(files_.size()); }
  // L0: ascending file number (= flush order). L1+: ascending key range, disjoint.
  [[nodiscard]] const std::vector<FilePtr>& files(int level) const { return files_[static_cast<size_t>(level)]; }
  [[nodiscard]] uint64_t LevelBytes(int level) const;
  [[nodiscard]] int NumFiles(int level) const { return static_cast<int>(files(level).size()); }

  // Point lookup through L0 (newest first) then L1..Ln. *found is set when an
  // entry (value or tombstone) for the key was found; *s is OK for a value,
  // NotFound for a tombstone.
  Status Get(const ReadOptions& ro, const LookupKey& key, sst::TableCache* tables, std::string* value,
             bool* found) const;

  // One iterator per L0 file and one concatenating iterator per deeper level.
  // The version must outlive them.
  void AddIterators(const ReadOptions& ro, sst::TableCache* tables,
                    std::vector<std::unique_ptr<Iterator>>* out) const;

  // Files in `level` whose user-key range intersects [begin, end]. For L0 the
  // range grows to cover every transitively overlapping file.
  [[nodiscard]] std::vector<FilePtr> GetOverlappingInputs(int level, const Slice& begin_user,
                                                          const Slice& end_user) const;
  [[nodiscard]] bool OverlapInLevel(int level, const Slice& begin_user, const Slice& end_user) const;

  // Set by VersionSet::Finalize.
  double compaction_score = -1;
  int compaction_level = -1;

 private:
  friend class VersionSet;
  std::vector<std::vector<FilePtr>> files_;
  std::vector<std::vector<FilePtr>> l0_singletons_;  // One-element views of files_[0].
};

// Concatenates the tables of one sorted level (L1+). The version that owns
// `files` must outlive the iterator.
std::unique_ptr<Iterator> NewLevelIterator(const ReadOptions& ro, sst::TableCache* tables,
                                           const std::vector<Version::FilePtr>* files);

class VersionSet {
 public:
  VersionSet(std::string dbname, const Options& options, sst::TableCache* tables);
  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  // Loads CURRENT -> MANIFEST. NotFound when the database does not exist.
  Status Recover();
  // Writes the first MANIFEST of a new database.
  Status CreateNew();

  // Applies `edit` to the current version and persists the result as a new
  // MANIFEST snapshot:
  //   1. write MANIFEST-<n> and fsync it,
  //   2. write <n>.tmp = "MANIFEST-<n>\n" and fsync it,
  //   3. renameat2(<n>.tmp -> CURRENT) (atomic replace),
  //   4. fsync the database directory,
  //   5. unlink the previous manifest.
  // On failure the in-memory state is unchanged; the caller must treat the
  // error as fatal (the on-disk CURRENT may point at either manifest).
  // Serialised internally; safe to call from multiple threads.
  Status LogAndApply(VersionEdit* edit);

  [[nodiscard]] std::shared_ptr<const Version> current() const;

  uint64_t NewFileNumber() noexcept { return next_file_number_.fetch_add(1, std::memory_order_relaxed); }
  void MarkFileNumberUsed(uint64_t n) noexcept;
  [[nodiscard]] uint64_t log_number() const noexcept { return log_number_.load(std::memory_order_acquire); }
  [[nodiscard]] uint64_t last_sequence() const noexcept { return last_sequence_.load(std::memory_order_acquire); }
  void SetLastSequence(uint64_t s) noexcept;
  [[nodiscard]] uint64_t manifest_number() const noexcept { return manifest_number_.load(std::memory_order_acquire); }

  [[nodiscard]] std::string CompactPointer(int level) const;
  [[nodiscard]] uint64_t MaxBytesForLevel(int level) const;
  [[nodiscard]] const Options& options() const noexcept { return options_; }

  // Every file referenced by any version still alive (held by readers,
  // iterators or compactions).
  void AddLiveFiles(std::set<uint64_t>* live) const;

 private:
  std::shared_ptr<Version> Apply(const Version& base, const VersionEdit& edit) const;
  void Finalize(Version* v) const;
  Status WriteSnapshot(const Version& v, uint64_t manifest_number, uint64_t log_number, uint64_t next_file,
                       uint64_t last_seq, const std::vector<std::string>& compact_pointers);
  void Install(std::shared_ptr<Version> v);

  const std::string dbname_;
  const Options options_;
  sst::TableCache* const tables_;

  std::mutex manifest_mu_;  // Serialises LogAndApply.
  mutable std::mutex mu_;   // Guards current_, all_, compact_pointers_.
  std::shared_ptr<const Version> current_;
  mutable std::vector<std::weak_ptr<const Version>> all_;
  std::vector<std::string> compact_pointers_;

  std::atomic<uint64_t> next_file_number_{2};
  std::atomic<uint64_t> log_number_{0};
  std::atomic<uint64_t> last_sequence_{0};
  std::atomic<uint64_t> manifest_number_{0};
};

}  // namespace lsm
