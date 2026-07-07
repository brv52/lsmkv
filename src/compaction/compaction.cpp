#include "compaction/compaction.hpp"

#include <algorithm>

#include "sstable/table_builder.hpp"
#include "util/file_util.hpp"

namespace lsm {

// ---------------------------------------------------------------------------
// MergingIterator

namespace {

class MergingIterator final : public Iterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> children) : children_(std::move(children)) {
    heap_.reserve(children_.size());
  }

  [[nodiscard]] bool Valid() const override { return !heap_.empty(); }
  [[nodiscard]] Slice key() const override { return heap_.front()->key(); }
  [[nodiscard]] Slice value() const override { return heap_.front()->value(); }
  [[nodiscard]] Status status() const override {
    for (const auto& c : children_) {
      if (Status s = c->status(); !s.ok()) return s;
    }
    return Status::OK();
  }

  void SeekToFirst() override {
    for (auto& c : children_) c->SeekToFirst();
    Rebuild();
  }
  void Seek(const Slice& target) override {
    for (auto& c : children_) c->Seek(target);
    Rebuild();
  }
  void Next() override {
    std::pop_heap(heap_.begin(), heap_.end(), Greater{});
    Iterator* cur = heap_.back();
    cur->Next();
    if (cur->Valid()) std::push_heap(heap_.begin(), heap_.end(), Greater{});
    else heap_.pop_back();
  }

 private:
  struct Greater {
    bool operator()(const Iterator* a, const Iterator* b) const noexcept {
      return InternalKeyComparator::Compare(a->key(), b->key()) > 0;
    }
  };

  void Rebuild() {
    heap_.clear();
    for (auto& c : children_) {
      if (c->Valid()) heap_.push_back(c.get());
    }
    std::make_heap(heap_.begin(), heap_.end(), Greater{});
  }

  std::vector<std::unique_ptr<Iterator>> children_;
  std::vector<Iterator*> heap_;  // Min-heap on the current key.
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children) {
  return std::make_unique<MergingIterator>(std::move(children));
}

// ---------------------------------------------------------------------------
// Compaction

bool Compaction::IsBaseLevelForKey(const Slice& user_key) {
  const int n = input_version->num_levels();
  if (level_ptrs_.empty()) level_ptrs_.assign(static_cast<size_t>(n), 0);
  for (int lvl = output_level + 1; lvl < n; ++lvl) {
    const auto& files = input_version->files(lvl);
    size_t& p = level_ptrs_[static_cast<size_t>(lvl)];
    while (p < files.size() && user_key.compare(ExtractUserKey(files[p]->largest)) > 0) ++p;
    if (p < files.size() && user_key.compare(ExtractUserKey(files[p]->smallest)) >= 0) return false;
  }
  return true;
}

void Compaction::AddInputDeletions(VersionEdit* edit) const {
  for (int which = 0; which < 2; ++which) {
    for (const auto& f : inputs[which]) edit->DeleteFile(level + which, f->number);
  }
}

uint64_t Compaction::InputBytes() const {
  uint64_t sum = 0;
  for (const auto& in : inputs) {
    for (const auto& f : in) sum += f->file_size;
  }
  return sum;
}

std::unique_ptr<Compaction> PickCompaction(const VersionSet& vset, int force_level) {
  auto v = vset.current();
  const Options& o = vset.options();
  int level;
  double score;
  if (force_level >= 0) {
    if (force_level >= v->num_levels() - 1 || v->files(force_level).empty()) return nullptr;
    level = force_level;
    score = 1.0;
  } else {
    if (v->compaction_score < 1.0 || v->compaction_level < 0) return nullptr;
    level = v->compaction_level;
    score = v->compaction_score;
  }

  auto c = std::make_unique<Compaction>();
  c->level = level;
  c->output_level = level + 1;
  c->input_version = v;
  c->max_output_file_size = o.target_file_size;
  c->score = score;

  const auto& files = v->files(level);
  if (level == 0) {
    // L0 files overlap arbitrarily; draining all of them at once keeps
    // read amplification bounded and avoids picking the same range twice.
    c->inputs[0] = files;
  } else {
    // Round-robin through the key space using the level's compact pointer.
    const std::string ptr = vset.CompactPointer(level);
    Version::FilePtr pick = files.front();
    if (!ptr.empty()) {
      for (const auto& f : files) {
        if (InternalKeyComparator::Compare(f->largest, ptr) > 0) {
          pick = f;
          break;
        }
      }
    }
    c->inputs[0].push_back(pick);
  }

  std::string lo = ExtractUserKey(c->inputs[0].front()->smallest).ToString();
  std::string hi = ExtractUserKey(c->inputs[0].front()->largest).ToString();
  for (const auto& f : c->inputs[0]) {
    const Slice s = ExtractUserKey(f->smallest), l = ExtractUserKey(f->largest);
    if (s.compare(lo) < 0) lo = s.ToString();
    if (l.compare(hi) > 0) hi = l.ToString();
  }
  c->inputs[1] = v->GetOverlappingInputs(c->output_level, lo, hi);
  return c;
}

// ---------------------------------------------------------------------------
// Output generation

namespace {

class OutputWriter {
 public:
  OutputWriter(const Options& o, const std::string& dbname, IoBackend* io) : o_(o), dbname_(dbname), io_(io) {}
  ~OutputWriter() { Discard(); }

  Status Open(uint64_t number) {
    meta_ = FileMetaData{};
    meta_.number = number;
    created_.push_back(number);
    return sst::TableBuilder::Create(o_, io_, TableFileName(dbname_, number), &builder_);
  }
  [[nodiscard]] bool is_open() const noexcept { return builder_ != nullptr; }
  [[nodiscard]] uint64_t size() const noexcept { return builder_ ? builder_->FileSize() : 0; }
  void Add(const Slice& k, const Slice& v) { builder_->Add(k, v); }

  Status Close(std::vector<FileMetaData>* outputs, uint64_t* bytes) {
    if (!builder_) return Status::OK();
    Status s = builder_->Finish();
    if (s.ok() && builder_->NumEntries() > 0) {
      meta_.file_size = builder_->FileSize();
      meta_.smallest = builder_->smallest_key();
      meta_.largest = builder_->largest_key();
      *bytes += meta_.file_size;
      outputs->push_back(meta_);
    }
    builder_.reset();
    return s;
  }

  // Keep files that were handed to the caller.
  void Commit() { created_.clear(); }

  void Discard() {
    if (builder_) {
      builder_->Abandon();
      builder_.reset();
    }
    for (uint64_t n : created_) (void)RemoveFile(TableFileName(dbname_, n));
    created_.clear();
  }

 private:
  const Options& o_;
  const std::string& dbname_;
  IoBackend* io_;
  std::unique_ptr<sst::TableBuilder> builder_;
  FileMetaData meta_;
  std::vector<uint64_t> created_;
};

}  // namespace

Status RunCompaction(Compaction* c, const Options& options, const std::string& dbname, IoBackend* io,
                     sst::TableCache* tables, const std::function<uint64_t()>& new_file_number,
                     const std::function<bool()>& should_stop, CompactionResult* result) {
  ReadOptions ro;
  ro.fill_cache = false;  // Don't let a compaction scan evict the hot set.
  ro.verify_checksums = true;
  ro.readahead_size = options.compaction_readahead_size;

  std::vector<std::vector<Version::FilePtr>> singles;  // Storage for per-L0-file views.
  std::vector<std::unique_ptr<Iterator>> children;
  if (c->level == 0) {
    singles.reserve(c->inputs[0].size());
    for (const auto& f : c->inputs[0]) singles.push_back({f});
    for (const auto& s : singles) children.push_back(NewLevelIterator(ro, tables, &s));
  } else {
    children.push_back(NewLevelIterator(ro, tables, &c->inputs[0]));
  }
  if (!c->inputs[1].empty()) children.push_back(NewLevelIterator(ro, tables, &c->inputs[1]));
  auto merged = NewMergingIterator(std::move(children));

  result->bytes_read = c->InputBytes();
  OutputWriter out(options, dbname, io);
  std::string current_user_key;
  bool has_current = false;

  for (merged->SeekToFirst(); merged->Valid(); merged->Next()) {
    if (should_stop()) return Status::Busy("compaction interrupted by shutdown");
    const Slice key = merged->key();
    const Slice ukey = ExtractUserKey(key);
    ++result->entries_in;

    // Without snapshots only the newest version of each user key is
    // observable; the merge yields it first.
    if (has_current && ukey == Slice(current_user_key)) {
      ++result->entries_dropped;
      continue;
    }
    current_user_key.assign(ukey.data(), ukey.size());
    has_current = true;
    if (ExtractValueType(key) == ValueType::kDeletion && c->IsBaseLevelForKey(ukey)) {
      ++result->entries_dropped;
      continue;
    }

    if (out.is_open() && out.size() >= c->max_output_file_size) {
      LSM_RETURN_IF_ERROR(out.Close(&result->outputs, &result->bytes_written));
    }
    if (!out.is_open()) LSM_RETURN_IF_ERROR(out.Open(new_file_number()));
    out.Add(key, merged->value());
  }
  LSM_RETURN_IF_ERROR(merged->status());
  LSM_RETURN_IF_ERROR(out.Close(&result->outputs, &result->bytes_written));
  out.Commit();
  return Status::OK();
}

Status BuildTable(const Options& options, const std::string& dbname, IoBackend* io, Iterator* it,
                  uint64_t file_number, FileMetaData* meta) {
  meta->number = file_number;
  meta->file_size = 0;
  it->SeekToFirst();
  if (!it->Valid()) return it->status();

  OutputWriter out(options, dbname, io);
  LSM_RETURN_IF_ERROR(out.Open(file_number));
  for (; it->Valid(); it->Next()) out.Add(it->key(), it->value());
  LSM_RETURN_IF_ERROR(it->status());
  std::vector<FileMetaData> outputs;
  uint64_t bytes = 0;
  LSM_RETURN_IF_ERROR(out.Close(&outputs, &bytes));
  if (outputs.size() != 1) return Status::Corruption("flush produced no table");
  out.Commit();
  *meta = std::move(outputs.front());
  return Status::OK();
}

}  // namespace lsm
