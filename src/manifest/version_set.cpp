#include "manifest/version_set.hpp"

#include <fcntl.h>
#include <stdio.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <set>

#include "util/crc32c.hpp"
#include "util/file_util.hpp"

namespace lsm {

namespace {

int CompareInternal(const Slice& a, const Slice& b) { return InternalKeyComparator::Compare(a, b); }

bool BeforeFile(const Slice& user_key, const FileMetaData& f) {
  return user_key.compare(ExtractUserKey(f.largest)) > 0;
}
bool AfterFile(const Slice& user_key, const FileMetaData& f) {
  return user_key.compare(ExtractUserKey(f.smallest)) < 0;
}

// Index of the first file in a sorted level whose largest key >= ikey.
size_t FindFile(const std::vector<Version::FilePtr>& files, const Slice& ikey) {
  size_t lo = 0, hi = files.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (CompareInternal(files[mid]->largest, ikey) < 0) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

Status GetFromFile(const ReadOptions& ro, sst::TableCache* tables, const FileMetaData& f, const LookupKey& key,
                   std::string* value, bool* found, Status* result) {
  std::shared_ptr<sst::TableReader> t;
  LSM_RETURN_IF_ERROR(tables->Get(f.number, &t));
  ValueType type{};
  bool hit = false;
  LSM_RETURN_IF_ERROR(t->Get(ro, key.internal_key(), &hit, &type, value));
  if (hit) {
    *found = true;
    *result = type == ValueType::kValue ? Status::OK() : Status::NotFound();
  }
  return Status::OK();
}

}  // namespace

// ---------------------------------------------------------------------------
// Version

uint64_t Version::LevelBytes(int level) const {
  uint64_t sum = 0;
  for (const auto& f : files(level)) sum += f->file_size;
  return sum;
}

Status Version::Get(const ReadOptions& ro, const LookupKey& key, sst::TableCache* tables, std::string* value,
                    bool* found) const {
  *found = false;
  const Slice user_key = key.user_key();
  Status result = Status::NotFound();

  const auto& l0 = files(0);
  for (auto it = l0.rbegin(); it != l0.rend(); ++it) {  // Newest flush first.
    const FileMetaData& f = **it;
    if (BeforeFile(user_key, f) || AfterFile(user_key, f)) continue;
    LSM_RETURN_IF_ERROR(GetFromFile(ro, tables, f, key, value, found, &result));
    if (*found) return result;
  }
  for (int level = 1; level < num_levels(); ++level) {
    const auto& fs = files(level);
    if (fs.empty()) continue;
    const size_t idx = FindFile(fs, key.internal_key());
    if (idx >= fs.size() || AfterFile(user_key, *fs[idx])) continue;
    LSM_RETURN_IF_ERROR(GetFromFile(ro, tables, *fs[idx], key, value, found, &result));
    if (*found) return result;
  }
  return Status::NotFound();
}

std::vector<Version::FilePtr> Version::GetOverlappingInputs(int level, const Slice& begin_user,
                                                            const Slice& end_user) const {
  std::vector<FilePtr> out;
  std::string lo = begin_user.ToString(), hi = end_user.ToString();
  const auto& fs = files(level);
  for (size_t i = 0; i < fs.size();) {
    const FileMetaData& f = *fs[i++];
    const Slice fs_user = ExtractUserKey(f.smallest), fl_user = ExtractUserKey(f.largest);
    if (fl_user.compare(lo) < 0 || fs_user.compare(hi) > 0) continue;
    out.push_back(fs[i - 1]);
    if (level == 0) {
      // L0 files overlap each other: widen the range and restart the scan.
      bool widened = false;
      if (fs_user.compare(lo) < 0) {
        lo = fs_user.ToString();
        widened = true;
      }
      if (fl_user.compare(hi) > 0) {
        hi = fl_user.ToString();
        widened = true;
      }
      if (widened) {
        out.clear();
        i = 0;
      }
    }
  }
  return out;
}

bool Version::OverlapInLevel(int level, const Slice& begin_user, const Slice& end_user) const {
  for (const auto& f : files(level)) {
    if (ExtractUserKey(f->largest).compare(begin_user) < 0 || ExtractUserKey(f->smallest).compare(end_user) > 0) {
      continue;
    }
    return true;
  }
  return false;
}

namespace {

class LevelIterator final : public Iterator {
 public:
  LevelIterator(const ReadOptions& ro, sst::TableCache* tables, const std::vector<Version::FilePtr>* files)
      : ro_(ro), tables_(tables), files_(files) {}

  [[nodiscard]] bool Valid() const override { return iter_ != nullptr && iter_->Valid(); }
  [[nodiscard]] Slice key() const override { return iter_->key(); }
  [[nodiscard]] Slice value() const override { return iter_->value(); }
  [[nodiscard]] Status status() const override {
    if (!status_.ok()) return status_;
    return iter_ ? iter_->status() : Status::OK();
  }

  void SeekToFirst() override {
    Open(0);
    if (iter_) iter_->SeekToFirst();
    SkipEmpty();
  }
  void Seek(const Slice& target) override {
    Open(FindFile(*files_, target));
    if (iter_) iter_->Seek(target);
    SkipEmpty();
  }
  void Next() override {
    iter_->Next();
    SkipEmpty();
  }

 private:
  void Open(size_t idx) {
    iter_.reset();
    table_.reset();
    index_ = idx;
    if (idx >= files_->size()) return;
    Status s = tables_->Get((*files_)[idx]->number, &table_);
    if (!s.ok()) {
      status_ = s;
      return;
    }
    iter_ = table_->NewIterator(ro_);
  }
  void SkipEmpty() {
    while (status_.ok() && (iter_ == nullptr || !iter_->Valid())) {
      if (iter_ && !iter_->status().ok()) return;
      if (index_ + 1 >= files_->size()) {
        iter_.reset();
        return;
      }
      Open(index_ + 1);
      if (iter_) iter_->SeekToFirst();
    }
  }

  const ReadOptions ro_;
  sst::TableCache* const tables_;
  const std::vector<Version::FilePtr>* const files_;
  size_t index_ = 0;
  std::shared_ptr<sst::TableReader> table_;
  std::unique_ptr<Iterator> iter_;
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewLevelIterator(const ReadOptions& ro, sst::TableCache* tables,
                                           const std::vector<Version::FilePtr>* files) {
  return std::make_unique<LevelIterator>(ro, tables, files);
}

void Version::AddIterators(const ReadOptions& ro, sst::TableCache* tables,
                           std::vector<std::unique_ptr<Iterator>>* out) const {
  // L0 files overlap, so each needs its own iterator; a single-file "level"
  // view keeps the table pinned for as long as the iterator lives.
  for (const auto& single : l0_singletons_) out->push_back(NewLevelIterator(ro, tables, &single));
  for (int level = 1; level < num_levels(); ++level) {
    if (!files(level).empty()) out->push_back(NewLevelIterator(ro, tables, &files_[static_cast<size_t>(level)]));
  }
}

// ---------------------------------------------------------------------------
// VersionSet

VersionSet::VersionSet(std::string dbname, const Options& options, sst::TableCache* tables)
    : dbname_(std::move(dbname)),
      options_(options),
      tables_(tables),
      current_(std::make_shared<Version>(options.num_levels)),
      compact_pointers_(static_cast<size_t>(options.num_levels)) {}

std::shared_ptr<const Version> VersionSet::current() const {
  std::lock_guard lk(mu_);
  return current_;
}

void VersionSet::MarkFileNumberUsed(uint64_t n) noexcept {
  uint64_t cur = next_file_number_.load(std::memory_order_relaxed);
  while (cur <= n && !next_file_number_.compare_exchange_weak(cur, n + 1, std::memory_order_relaxed)) {
  }
}

void VersionSet::SetLastSequence(uint64_t s) noexcept {
  uint64_t cur = last_sequence_.load(std::memory_order_relaxed);
  while (cur < s && !last_sequence_.compare_exchange_weak(cur, s, std::memory_order_release)) {
  }
}

std::string VersionSet::CompactPointer(int level) const {
  std::lock_guard lk(mu_);
  return compact_pointers_[static_cast<size_t>(level)];
}

uint64_t VersionSet::MaxBytesForLevel(int level) const {
  double r = static_cast<double>(options_.max_bytes_for_level_base);
  for (int l = 1; l < level; ++l) r *= options_.max_bytes_for_level_multiplier;
  return static_cast<uint64_t>(r);
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) const {
  std::lock_guard lk(mu_);
  std::erase_if(all_, [](const std::weak_ptr<const Version>& w) { return w.expired(); });
  for (const auto& w : all_) {
    if (auto v = w.lock()) {
      for (int level = 0; level < v->num_levels(); ++level) {
        for (const auto& f : v->files(level)) live->insert(f->number);
      }
    }
  }
}

std::shared_ptr<Version> VersionSet::Apply(const Version& base, const VersionEdit& edit) const {
  auto v = std::make_shared<Version>(options_.num_levels);
  std::set<std::pair<int, uint64_t>> deleted(edit.deleted_files().begin(), edit.deleted_files().end());
  for (int level = 0; level < base.num_levels() && level < v->num_levels(); ++level) {
    for (const auto& f : base.files(level)) {
      if (!deleted.contains({level, f->number})) v->files_[static_cast<size_t>(level)].push_back(f);
    }
  }
  for (const auto& [level, f] : edit.new_files()) {
    if (level < 0 || level >= v->num_levels()) continue;
    v->files_[static_cast<size_t>(level)].push_back(std::make_shared<const FileMetaData>(f));
  }
  std::sort(v->files_[0].begin(), v->files_[0].end(),
            [](const Version::FilePtr& a, const Version::FilePtr& b) { return a->number < b->number; });
  for (int level = 1; level < v->num_levels(); ++level) {
    auto& fs = v->files_[static_cast<size_t>(level)];
    std::sort(fs.begin(), fs.end(), [](const Version::FilePtr& a, const Version::FilePtr& b) {
      return CompareInternal(a->smallest, b->smallest) < 0;
    });
  }
  v->l0_singletons_.clear();
  for (const auto& f : v->files_[0]) v->l0_singletons_.push_back({f});
  Finalize(v.get());
  return v;
}

void VersionSet::Finalize(Version* v) const {
  double best = -1;
  int best_level = -1;
  for (int level = 0; level < v->num_levels() - 1; ++level) {
    double score;
    if (level == 0) {
      score = static_cast<double>(v->NumFiles(0)) / std::max(1, options_.level0_compaction_trigger);
    } else {
      score = static_cast<double>(v->LevelBytes(level)) / static_cast<double>(MaxBytesForLevel(level));
    }
    if (score > best) {
      best = score;
      best_level = level;
    }
  }
  v->compaction_score = best;
  v->compaction_level = best_level;
}

Status VersionSet::WriteSnapshot(const Version& v, uint64_t manifest_number, uint64_t log_number,
                                 uint64_t next_file, uint64_t last_seq,
                                 const std::vector<std::string>& compact_pointers) {
  VersionEdit snap;
  snap.SetLogNumber(log_number);
  snap.SetNextFile(next_file);
  snap.SetLastSequence(last_seq);
  for (size_t level = 0; level < compact_pointers.size(); ++level) {
    if (!compact_pointers[level].empty()) snap.SetCompactPointer(static_cast<int>(level), compact_pointers[level]);
  }
  for (int level = 0; level < v.num_levels(); ++level) {
    for (const auto& f : v.files(level)) snap.AddFile(level, *f);
  }
  std::string payload;
  snap.EncodeTo(&payload);
  std::string record;
  PutFixed32(&record, crc32c::Mask(crc32c::Value(payload.data(), payload.size())));
  PutFixed32(&record, static_cast<uint32_t>(payload.size()));
  record += payload;

  const std::string manifest = ManifestFileName(dbname_, manifest_number);
  LSM_RETURN_IF_ERROR(WriteFileSync(manifest, record));  // (1)

  char name[64];
  std::snprintf(name, sizeof(name), "MANIFEST-%06" PRIu64 "\n", manifest_number);
  char tmp_name[64];
  std::snprintf(tmp_name, sizeof(tmp_name), "/%06" PRIu64 ".tmp", manifest_number);
  const std::string tmp = dbname_ + tmp_name;
  LSM_RETURN_IF_ERROR(WriteFileSync(tmp, name));  // (2)

  const std::string current = CurrentFileName(dbname_);
  if (::renameat2(AT_FDCWD, tmp.c_str(), AT_FDCWD, current.c_str(), 0) != 0) {  // (3)
    if (errno != ENOSYS && errno != EINVAL) return Status::FromErrno("renameat2 CURRENT");
    if (::rename(tmp.c_str(), current.c_str()) != 0) return Status::FromErrno("rename CURRENT");
  }
  return FsyncDir(dbname_);  // (4)
}

void VersionSet::Install(std::shared_ptr<Version> v) {
  std::lock_guard lk(mu_);
  current_ = v;
  std::erase_if(all_, [](const std::weak_ptr<const Version>& w) { return w.expired(); });
  all_.push_back(std::move(v));
}

Status VersionSet::LogAndApply(VersionEdit* edit) {
  std::lock_guard serial(manifest_mu_);
  const auto base = current();

  const uint64_t log_number = edit->log_number().value_or(log_number_.load(std::memory_order_acquire));
  const uint64_t last_seq = std::max(edit->last_sequence().value_or(0), last_sequence());
  std::vector<std::string> pointers;
  {
    std::lock_guard lk(mu_);
    pointers = compact_pointers_;
  }
  for (const auto& [level, key] : edit->compact_pointers()) {
    if (level >= 0 && static_cast<size_t>(level) < pointers.size()) pointers[static_cast<size_t>(level)] = key;
  }

  auto v = Apply(*base, *edit);
  const uint64_t manifest_number = NewFileNumber();
  const uint64_t next_file = next_file_number_.load(std::memory_order_relaxed);
  edit->SetLogNumber(log_number);
  edit->SetNextFile(next_file);
  edit->SetLastSequence(last_seq);

  Status s = WriteSnapshot(*v, manifest_number, log_number, next_file, last_seq, pointers);
  if (!s.ok()) return s;

  const uint64_t old_manifest = manifest_number_.exchange(manifest_number, std::memory_order_acq_rel);
  log_number_.store(log_number, std::memory_order_release);
  SetLastSequence(last_seq);
  {
    std::lock_guard lk(mu_);
    compact_pointers_ = std::move(pointers);
  }
  Install(std::move(v));
  if (old_manifest != 0) (void)RemoveFile(ManifestFileName(dbname_, old_manifest));
  return Status::OK();
}

Status VersionSet::CreateNew() {
  VersionEdit edit;
  edit.SetLogNumber(0);
  edit.SetLastSequence(0);
  return LogAndApply(&edit);
}

Status VersionSet::Recover() {
  std::string current;
  Status s = ReadFileToString(CurrentFileName(dbname_), &current);
  if (s.IsNotFound()) return s;
  LSM_RETURN_IF_ERROR(s);
  if (current.empty() || current.back() != '\n' || current.rfind("MANIFEST-", 0) != 0) {
    return Status::Corruption("CURRENT file is malformed");
  }
  current.pop_back();
  uint64_t manifest_number = 0;
  FileType type{};
  if (!ParseFileName(current, &manifest_number, &type) || type != FileType::kManifest) {
    return Status::Corruption("CURRENT names an invalid manifest: " + current);
  }

  std::string contents;
  s = ReadFileToString(dbname_ + "/" + current, &contents);
  if (s.IsNotFound()) return Status::Corruption("CURRENT points to missing " + current);
  LSM_RETURN_IF_ERROR(s);

  std::shared_ptr<Version> v = std::make_shared<Version>(options_.num_levels);
  Finalize(v.get());
  std::optional<uint64_t> log_number, next_file, last_seq;
  std::vector<std::string> pointers(static_cast<size_t>(options_.num_levels));
  Slice in(contents);
  int records = 0;
  while (!in.empty()) {
    if (in.size() < 8) return Status::Corruption("truncated manifest record header");
    const uint32_t crc = crc32c::Unmask(DecodeFixed32(in.data()));
    const uint32_t len = DecodeFixed32(in.data() + 4);
    if (in.size() - 8 < len) return Status::Corruption("truncated manifest record");
    const Slice payload(in.data() + 8, len);
    if (crc32c::Value(payload.data(), payload.size()) != crc) return Status::Corruption("manifest checksum mismatch");
    VersionEdit edit;
    LSM_RETURN_IF_ERROR(edit.DecodeFrom(payload));
    v = Apply(*v, edit);
    if (edit.log_number()) log_number = edit.log_number();
    if (edit.next_file_number()) next_file = edit.next_file_number();
    if (edit.last_sequence()) last_seq = edit.last_sequence();
    for (const auto& [level, key] : edit.compact_pointers()) {
      if (level >= 0 && level < options_.num_levels) pointers[static_cast<size_t>(level)] = key;
    }
    in.remove_prefix(8 + len);
    ++records;
  }
  if (records == 0 || !next_file || !log_number || !last_seq) {
    return Status::Corruption("manifest is missing required fields");
  }

  manifest_number_.store(manifest_number, std::memory_order_release);
  next_file_number_.store(*next_file, std::memory_order_relaxed);
  MarkFileNumberUsed(manifest_number);
  log_number_.store(*log_number, std::memory_order_release);
  last_sequence_.store(*last_seq, std::memory_order_release);
  {
    std::lock_guard lk(mu_);
    compact_pointers_ = std::move(pointers);
  }
  Install(std::move(v));
  return Status::OK();
}

}  // namespace lsm
