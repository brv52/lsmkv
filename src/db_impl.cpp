#include "db_impl.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "wal/log_reader.hpp"

namespace lsm {

namespace {

Status ValidateBatch(const WriteBatch& batch) {
  for (const auto& op : batch.ops()) {
    if (op.key.size() > wal::kMaxKeySize) return Status::InvalidArgument("key longer than 65535 bytes");
  }
  return Status::OK();
}

// Snapshot iterator: merges memtables and tables, hides entries newer than
// the snapshot, shadowed versions and tombstones.
class DBIter final : public Iterator {
 public:
  DBIter(std::unique_ptr<Iterator> merged, SequenceNumber snapshot, std::shared_ptr<MemTable> mem,
         std::deque<std::shared_ptr<MemTable>> imm, std::shared_ptr<const Version> version)
      : iter_(std::move(merged)),
        snapshot_(snapshot),
        mem_(std::move(mem)),
        imm_(std::move(imm)),
        version_(std::move(version)) {}

  ~DBIter() override {
    iter_.reset();  // Children reference the pinned memtables/version.
  }

  [[nodiscard]] bool Valid() const override { return valid_; }
  [[nodiscard]] Slice key() const override { return ExtractUserKey(iter_->key()); }
  [[nodiscard]] Slice value() const override { return iter_->value(); }
  [[nodiscard]] Status status() const override { return iter_->status(); }

  void SeekToFirst() override {
    iter_->SeekToFirst();
    FindNextUserEntry(false);
  }
  void Seek(const Slice& target) override {
    const std::string ikey = MakeInternalKey(target, snapshot_, kValueTypeForSeek);
    iter_->Seek(ikey);
    FindNextUserEntry(false);
  }
  void Next() override {
    const Slice cur = ExtractUserKey(iter_->key());
    skip_.assign(cur.data(), cur.size());
    iter_->Next();
    FindNextUserEntry(true);
  }

 private:
  void FindNextUserEntry(bool skipping) {
    for (; iter_->Valid(); iter_->Next()) {
      const Slice ikey = iter_->key();
      if (ikey.size() < 8) continue;
      if (ExtractSequence(ikey) > snapshot_) continue;
      const Slice ukey = ExtractUserKey(ikey);
      if (skipping && ukey == Slice(skip_)) continue;  // Older version of a key already handled.
      if (ExtractValueType(ikey) == ValueType::kDeletion) {
        skip_.assign(ukey.data(), ukey.size());
        skipping = true;
        continue;
      }
      valid_ = true;
      return;
    }
    valid_ = false;
  }

  std::unique_ptr<Iterator> iter_;
  const SequenceNumber snapshot_;
  std::shared_ptr<MemTable> mem_;
  std::deque<std::shared_ptr<MemTable>> imm_;
  std::shared_ptr<const Version> version_;
  std::string skip_;
  bool valid_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle

DBImpl::DBImpl(const Options& options, std::string dbname, std::shared_ptr<IoBackend> io)
    : options_(options),
      dbname_(std::move(dbname)),
      io_(std::move(io)),
      block_cache_(std::make_unique<LruCache>(options.block_cache_capacity, options.block_cache_shards_bits)),
      table_cache_(
          std::make_unique<sst::TableCache>(options_, dbname_, io_.get(), block_cache_.get(), &table_stats_)),
      versions_(std::make_unique<VersionSet>(dbname_, options_, table_cache_.get())),
      queue_(std::make_unique<wal::GroupCommitQueue>([this](Group& g) { return LogGroup(g); },
                                                     [this](Writer& w, Group& g) { return ApplyWriter(w, g); },
                                                     options.wal_max_group_bytes)) {}

DBImpl::~DBImpl() {
  {
    std::lock_guard lk(mu_);
    shutting_down_.store(true, std::memory_order_release);
    bg_cv_.notify_all();
    state_cv_.notify_all();
  }
  if (flush_thread_.joinable()) flush_thread_.join();
  if (compaction_thread_.joinable()) compaction_thread_.join();
  if (log_) {
    if (panicked()) {
      log_->Abandon();  // Never fsync a descriptor that already failed.
    } else if (Status s = log_->Close(); !s.ok()) {
      std::fprintf(stderr, "lsmkv: WAL sync on close failed: %s\n", s.ToString().c_str());
      if (options_.abort_on_fsync_error && s.IsIOError()) std::abort();
    }
  }
}

void DBImpl::Panic(const Status& cause, const char* where) {
  if (options_.abort_on_fsync_error && cause.IsIOError()) {
    // fsyncgate: after a failed fsync the page cache may already claim the
    // data is clean. The only safe continuation is a restart that recovers
    // from what is provably durable (WAL + MANIFEST), so stop right here —
    // before the error is published, so no other thread can observe it and
    // carry on (e.g. a caller of Flush() returning an error status).
    std::fprintf(stderr, "lsmkv: PANIC in %s: %s — aborting process (abort_on_fsync_error)\n", where,
                 cause.ToString().c_str());
    std::fflush(stderr);
    std::abort();
  }
  {
    std::lock_guard lk(mu_);
    if (bg_error_.ok()) {
      bg_error_ = Status::Panic(std::string(where) + ": " + cause.ToString(), cause.err());
    }
    panicked_.store(true, std::memory_order_release);
    state_cv_.notify_all();
    bg_cv_.notify_all();
  }
  std::fprintf(stderr, "lsmkv: PANIC in %s: %s — database is read-only from now on\n", where,
               cause.ToString().c_str());
}

Status DBImpl::BackgroundError() const {
  std::lock_guard lk(mu_);
  return bg_error_;
}

// ---------------------------------------------------------------------------
// Recovery

Status DBImpl::Recover() {
  LSM_RETURN_IF_ERROR(CreateDirIfMissing(dbname_));
  LSM_RETURN_IF_ERROR(LockDB(dbname_, &db_lock_));

  Status s = versions_->Recover();
  if (s.IsNotFound()) {
    if (!options_.create_if_missing) return Status::InvalidArgument(dbname_ + " does not exist");
    LSM_RETURN_IF_ERROR(versions_->CreateNew());
  } else if (!s.ok()) {
    return s;
  } else if (options_.error_if_exists) {
    return Status::InvalidArgument(dbname_ + " already exists");
  }

  std::vector<std::string> names;
  LSM_RETURN_IF_ERROR(ListDir(dbname_, &names));
  std::vector<uint64_t> logs;
  for (const auto& n : names) {
    uint64_t number;
    FileType type;
    if (!ParseFileName(n, &number, &type)) continue;
    versions_->MarkFileNumberUsed(number);  // WALs are created without a manifest write.
    if (type == FileType::kLog && number >= versions_->log_number()) logs.push_back(number);
  }
  std::sort(logs.begin(), logs.end());

  SequenceNumber max_seq = versions_->last_sequence();
  VersionEdit edit;
  auto mem = std::make_shared<MemTable>();
  auto flush_recovered = [&]() -> Status {
    if (mem->empty()) return Status::OK();
    const uint64_t number = versions_->NewFileNumber();
    FileMetaData meta;
    auto it = mem->NewIterator();
    LSM_RETURN_IF_ERROR(BuildTable(options_, dbname_, io_.get(), it.get(), number, &meta));
    if (meta.file_size > 0) edit.AddFile(0, meta);
    mem = std::make_shared<MemTable>();
    return Status::OK();
  };

  for (uint64_t log_number : logs) {
    std::unique_ptr<wal::LogReader> reader;
    LSM_RETURN_IF_ERROR(wal::LogReader::Open(io_.get(), LogFileName(dbname_, log_number), options_, &reader));
    WriteBatch batch;
    for (;;) {
      uint64_t first_seq = 0;
      bool eof = false;
      LSM_RETURN_IF_ERROR(reader->ReadBatch(&batch, &first_seq, &eof));
      if (eof) break;
      SequenceNumber seq = first_seq;
      for (const auto& op : batch.ops()) mem->Add(seq++, op.type, op.key, op.value);
      if (batch.Count() > 0) max_seq = std::max(max_seq, first_seq + batch.Count() - 1);
      if (mem->ApproximateMemoryUsage() > options_.write_buffer_size) LSM_RETURN_IF_ERROR(flush_recovered());
    }
    if (!reader->tail_reason().empty()) {
      std::fprintf(stderr, "lsmkv: recovery: WAL %06llu ends at offset %llu (%s); later bytes ignored\n",
                   static_cast<unsigned long long>(log_number),
                   static_cast<unsigned long long>(reader->good_offset()), reader->tail_reason().c_str());
    }
  }
  LSM_RETURN_IF_ERROR(flush_recovered());
  if (!edit.new_files().empty()) LSM_RETURN_IF_ERROR(FsyncDir(dbname_));

  // Start a fresh WAL; once the manifest names it, the replayed logs are
  // obsolete (their contents now live in L0 tables).
  const uint64_t new_log = versions_->NewFileNumber();
  LSM_RETURN_IF_ERROR(wal::LogWriter::Create(io_.get(), LogFileName(dbname_, new_log), new_log, options_, &log_));
  edit.SetLogNumber(new_log);
  edit.SetLastSequence(max_seq);
  LSM_RETURN_IF_ERROR(versions_->LogAndApply(&edit));

  mem_ = std::make_shared<MemTable>();
  mem_->log_number = new_log;
  queue_->SetLastSequence(max_seq);
  DeleteObsoleteFiles();
  return Status::OK();
}

void DBImpl::StartBackgroundThreads() {
  flush_thread_ = std::thread([this] { FlushThread(); });
  compaction_thread_ = std::thread([this] { CompactionThread(); });
  std::lock_guard lk(mu_);
  bg_cv_.notify_all();
}

Status DB::Open(const Options& options, const std::string& path, std::unique_ptr<DB>* db) {
  db->reset();
  if (options.block_size < 512 || options.num_levels < 2 || options.write_buffer_size < 64 * 1024) {
    return Status::InvalidArgument("options out of range");
  }
  Status s;
  auto io = NewIoBackend(options, &s);
  if (!io) return s;
  auto impl = std::make_unique<DBImpl>(options, path, std::move(io));
  LSM_RETURN_IF_ERROR(impl->Recover());
  impl->StartBackgroundThreads();
  *db = std::move(impl);
  return Status::OK();
}

Status DB::Destroy(const std::string& path) {
  std::vector<std::string> names;
  Status s = ListDir(path, &names);
  if (!s.ok()) return s.IsIOError() && s.err() == ENOENT ? Status::OK() : s;
  for (const auto& n : names) {
    uint64_t number;
    FileType type;
    if (ParseFileName(n, &number, &type)) (void)RemoveFile(path + "/" + n);
  }
  ::rmdir(path.c_str());
  return Status::OK();
}

// ---------------------------------------------------------------------------
// Write path

Status DBImpl::Put(const WriteOptions& opts, const Slice& key, const Slice& value) {
  WriteBatch b;
  b.Put(key, value);
  return Write(opts, b);
}

Status DBImpl::Delete(const WriteOptions& opts, const Slice& key) {
  WriteBatch b;
  b.Delete(key);
  return Write(opts, b);
}

Status DBImpl::Write(const WriteOptions& opts, const WriteBatch& batch) {
  LSM_RETURN_IF_ERROR(ValidateBatch(batch));
  if (panicked()) return BackgroundError();
  Writer w;
  w.batch = &batch;
  w.sync = opts.sync && !opts.disable_wal;
  w.disable_wal = opts.disable_wal;
  Status s = queue_->Commit(&w);
  if (s.ok()) user_bytes_.fetch_add(batch.ApproximateBytes(), std::memory_order_relaxed);
  return s;
}

Status DBImpl::LogGroup(Group& g) {
  if (panicked()) return BackgroundError();
  {
    std::unique_lock lk(mu_);
    LSM_RETURN_IF_ERROR(MakeRoomForWrite(lk, g));
    g.context = mem_;
  }

  std::vector<wal::LogWriter::BatchRef> batches;
  batches.reserve(g.writers.size());
  for (Writer* w : g.writers) {
    if (!w->disable_wal && w->batch->Count() > 0) batches.push_back({w->batch, w->first_seq});
  }
  const uint64_t before = log_->bytes_submitted();
  Status s;
  if (!batches.empty()) s = log_->AddGroup(batches, g.sync);
  else if (g.sync) s = log_->Sync();
  wal_bytes_.fetch_add(log_->bytes_submitted() - before, std::memory_order_relaxed);
  if (!s.ok()) {
    if (s.IsInvalidArgument()) return s;  // Rejected before anything was written.
    log_->Abandon();
    Panic(s, "WAL append/fdatasync");
    return BackgroundError();
  }
  return Status::OK();
}

Status DBImpl::ApplyWriter(Writer& w, Group& g) {
  auto* mem = static_cast<MemTable*>(g.context.get());
  SequenceNumber seq = w.first_seq;
  for (const auto& op : w.batch->ops()) mem->Add(seq++, op.type, op.key, op.value);
  return Status::OK();
}

Status DBImpl::MakeRoomForWrite(std::unique_lock<std::mutex>& lk, Group& g) {
  bool delayed = false;
  for (;;) {
    if (!bg_error_.ok()) return bg_error_;
    if (shutting_down_.load(std::memory_order_acquire)) return Status::Busy("database is closing");
    const int l0 = versions_->current()->NumFiles(0);
    const bool force = force_switch_.load(std::memory_order_acquire);

    if (!force && !delayed && l0 >= options_.level0_slowdown_writes_trigger) {
      // Gentle back-pressure: give compaction a millisecond per group.
      delayed = true;
      write_stalls_.fetch_add(1, std::memory_order_relaxed);
      lk.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      lk.lock();
      continue;
    }
    if (!force && mem_->ApproximateMemoryUsage() < options_.write_buffer_size) {
      if (l0 >= options_.level0_stop_writes_trigger) {
        write_stalls_.fetch_add(1, std::memory_order_relaxed);
        state_cv_.wait(lk);
        continue;
      }
      return Status::OK();
    }
    if (force && mem_->empty()) {
      force_switch_.store(false, std::memory_order_release);
      return Status::OK();
    }
    if (static_cast<int>(imm_.size()) >= std::max(1, options_.max_immutable_memtables) ||
        l0 >= options_.level0_stop_writes_trigger) {
      write_stalls_.fetch_add(1, std::memory_order_relaxed);
      state_cv_.wait(lk);
      continue;
    }

    // Switch to a new memtable + WAL. We are the only leader, so log_ and
    // mem_ cannot change underneath us while the lock is dropped.
    const uint64_t new_log_number = versions_->NewFileNumber();
    std::unique_ptr<wal::LogWriter> new_log;
    lk.unlock();
    Status s = wal::LogWriter::Create(io_.get(), LogFileName(dbname_, new_log_number), new_log_number, options_,
                                      &new_log);
    if (s.ok()) {
      // Closing syncs the old WAL so it is complete before the memtable it
      // backs is handed to the flush thread.
      s = log_->Close();
    }
    lk.lock();
    if (!s.ok()) {
      lk.unlock();
      Panic(s, "WAL switch");
      lk.lock();
      return bg_error_;
    }
    log_ = std::move(new_log);
    mem_->last_sequence = g.first_seq - 1;
    imm_.push_back(std::move(mem_));
    mem_ = std::make_shared<MemTable>();
    mem_->log_number = new_log_number;
    force_switch_.store(false, std::memory_order_release);
    bg_cv_.notify_all();
    return Status::OK();
  }
}

// ---------------------------------------------------------------------------
// Read path

Status DBImpl::Get(const ReadOptions& opts, const Slice& key, std::string* value) {
  // Snapshot first: everything <= snapshot is already applied, and it is
  // still reachable from the structures captured afterwards (a flush
  // installs its version before popping the immutable memtable).
  const SequenceNumber snapshot = queue_->LastPublished();
  std::shared_ptr<MemTable> mem;
  std::deque<std::shared_ptr<MemTable>> imm;
  std::shared_ptr<const Version> version;
  {
    std::lock_guard lk(mu_);
    mem = mem_;
    imm = imm_;
    version = versions_->current();
  }
  const LookupKey lkey(key, snapshot);
  Status s;
  if (mem->Get(lkey, value, &s)) return s;
  for (auto it = imm.rbegin(); it != imm.rend(); ++it) {
    if ((*it)->Get(lkey, value, &s)) return s;
  }
  bool found = false;
  s = version->Get(opts, lkey, table_cache_.get(), value, &found);
  if (!found && s.ok()) return Status::NotFound();
  return s;
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& opts) {
  const SequenceNumber snapshot = queue_->LastPublished();
  std::shared_ptr<MemTable> mem;
  std::deque<std::shared_ptr<MemTable>> imm;
  std::shared_ptr<const Version> version;
  {
    std::lock_guard lk(mu_);
    mem = mem_;
    imm = imm_;
    version = versions_->current();
  }
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(mem->NewIterator());
  for (auto it = imm.rbegin(); it != imm.rend(); ++it) children.push_back((*it)->NewIterator());
  version->AddIterators(opts, table_cache_.get(), &children);
  return std::make_unique<DBIter>(NewMergingIterator(std::move(children)), snapshot, std::move(mem),
                                  std::move(imm), std::move(version));
}

// ---------------------------------------------------------------------------
// Flush

void DBImpl::FlushThread() {
  std::unique_lock lk(mu_);
  for (;;) {
    bg_cv_.wait(lk, [&] { return shutting_down_.load() || (!imm_.empty() && bg_error_.ok()); });
    if (shutting_down_.load()) return;  // Unflushed memtables are still in their (synced) WALs.
    std::shared_ptr<MemTable> imm = imm_.front();
    lk.unlock();
    Status s = FlushMemTable(imm);
    lk.lock();
    if (!s.ok()) {
      lk.unlock();
      Panic(s, "memtable flush");
      lk.lock();
      continue;
    }
    imm_.pop_front();
    state_cv_.notify_all();
    bg_cv_.notify_all();  // L0 grew: maybe compact.
  }
}

Status DBImpl::FlushMemTable(const std::shared_ptr<MemTable>& imm) {
  // Inserts from groups logged before the switch may still be running.
  queue_->WaitForPublished(imm->last_sequence);

  const uint64_t number = versions_->NewFileNumber();
  {
    std::lock_guard lk(mu_);
    pending_outputs_.insert(number);
  }
  FileMetaData meta;
  auto it = imm->NewIterator();
  Status s = BuildTable(options_, dbname_, io_.get(), it.get(), number, &meta);
  if (s.ok() && meta.file_size > 0) s = FsyncDir(dbname_);  // Table dirent durable before the manifest names it.

  if (s.ok()) {
    VersionEdit edit;
    if (meta.file_size > 0) edit.AddFile(0, meta);
    uint64_t next_log;
    {
      std::lock_guard lk(mu_);
      // The oldest WAL still needed belongs to the next memtable in line.
      next_log = imm_.size() > 1 ? imm_[1]->log_number : mem_->log_number;
    }
    edit.SetLogNumber(next_log);
    edit.SetLastSequence(imm->last_sequence);
    s = versions_->LogAndApply(&edit);
  }
  {
    std::lock_guard lk(mu_);
    pending_outputs_.erase(number);
  }
  if (s.ok()) {
    flush_bytes_.fetch_add(meta.file_size, std::memory_order_relaxed);
    DeleteObsoleteFiles();
  }
  return s;
}

Status DBImpl::Flush() {
  if (panicked()) return BackgroundError();
  {
    std::lock_guard lk(mu_);
    if (mem_->empty() && imm_.empty()) return Status::OK();
  }
  // Route the switch through the group-commit leader (the only thread that
  // may touch mem_/log_) by committing an empty batch.
  force_switch_.store(true, std::memory_order_release);
  WriteBatch empty;
  Writer w;
  w.batch = &empty;
  LSM_RETURN_IF_ERROR(queue_->Commit(&w));
  std::unique_lock lk(mu_);
  state_cv_.wait(lk, [&] { return imm_.empty() || !bg_error_.ok() || shutting_down_.load(); });
  return bg_error_;
}

// ---------------------------------------------------------------------------
// Compaction

bool DBImpl::NeedsCompaction() const {
  if (options_.disable_auto_compactions) return false;
  return versions_->current()->compaction_score >= 1.0;
}

void DBImpl::CompactionThread() {
  std::unique_lock lk(mu_);
  for (;;) {
    bg_cv_.wait(lk, [&] { return shutting_down_.load() || (bg_error_.ok() && NeedsCompaction()); });
    if (shutting_down_.load()) return;
    compaction_running_ = true;
    lk.unlock();
    Status s;
    {
      std::lock_guard cl(compaction_mu_);
      if (auto c = PickCompaction(*versions_)) s = DoCompaction(c.get());
    }
    lk.lock();
    compaction_running_ = false;
    state_cv_.notify_all();
    if (!s.ok() && !s.IsBusy()) {
      lk.unlock();
      Panic(s, "compaction");
      lk.lock();
    }
  }
}

Status DBImpl::DoCompaction(Compaction* c) {
  VersionEdit edit;
  if (c->IsTrivialMove()) {
    const auto& f = c->inputs[0].front();
    edit.DeleteFile(c->level, f->number);
    edit.AddFile(c->output_level, *f);
    edit.SetCompactPointer(c->level, f->largest);
    return versions_->LogAndApply(&edit);
  }

  std::vector<uint64_t> allocated;
  auto new_file_number = [&] {
    const uint64_t n = versions_->NewFileNumber();
    std::lock_guard lk(mu_);
    pending_outputs_.insert(n);
    allocated.push_back(n);
    return n;
  };
  auto should_stop = [&] { return shutting_down_.load(std::memory_order_relaxed); };
  CompactionResult result;
  Status s = RunCompaction(c, options_, dbname_, io_.get(), table_cache_.get(), new_file_number, should_stop,
                           &result);
  if (s.ok() && !result.outputs.empty()) s = FsyncDir(dbname_);
  if (s.ok()) {
    c->AddInputDeletions(&edit);
    for (const auto& out : result.outputs) edit.AddFile(c->output_level, out);
    std::string largest;
    for (const auto& f : c->inputs[0]) {
      if (largest.empty() || InternalKeyComparator::Compare(f->largest, largest) > 0) largest = f->largest;
    }
    edit.SetCompactPointer(c->level, largest);
    s = versions_->LogAndApply(&edit);
  }
  {
    std::lock_guard lk(mu_);
    for (uint64_t n : allocated) pending_outputs_.erase(n);
  }
  if (s.ok()) {
    compaction_bytes_read_.fetch_add(result.bytes_read, std::memory_order_relaxed);
    compaction_bytes_written_.fetch_add(result.bytes_written, std::memory_order_relaxed);
    DeleteObsoleteFiles();
  } else {
    for (const auto& out : result.outputs) (void)RemoveFile(TableFileName(dbname_, out.number));
  }
  return s;
}

Status DBImpl::WaitForCompactions() {
  std::unique_lock lk(mu_);
  state_cv_.wait(lk, [&] {
    return !bg_error_.ok() || shutting_down_.load() ||
           (imm_.empty() && !compaction_running_ && !NeedsCompaction());
  });
  return bg_error_;
}

Status DBImpl::CompactAll() {
  LSM_RETURN_IF_ERROR(Flush());
  std::lock_guard cl(compaction_mu_);
  for (int level = 0; level < options_.num_levels - 1; ++level) {
    while (auto c = PickCompaction(*versions_, level)) {
      if (Status s = DoCompaction(c.get()); !s.ok()) {
        if (!s.IsBusy()) Panic(s, "manual compaction");
        return s;
      }
    }
  }
  return BackgroundError();
}

void DBImpl::DeleteObsoleteFiles() {
  // Order matters. A file in the listing was created after its number went
  // into pending_outputs_; it leaves that set only after the version naming
  // it is installed. Reading pending *after* the listing and live versions
  // *after* pending therefore always covers every in-flight output.
  std::vector<std::string> names;
  if (!ListDir(dbname_, &names).ok()) return;
  std::set<uint64_t> live;
  {
    std::lock_guard lk(mu_);
    live = pending_outputs_;
  }
  versions_->AddLiveFiles(&live);
  const uint64_t min_log = versions_->log_number();
  const uint64_t manifest = versions_->manifest_number();
  for (const auto& n : names) {
    uint64_t number;
    FileType type;
    if (!ParseFileName(n, &number, &type)) continue;
    bool keep = true;
    switch (type) {
      case FileType::kLog: keep = number >= min_log; break;
      case FileType::kTable: keep = live.contains(number); break;
      case FileType::kManifest: keep = number >= manifest; break;
      case FileType::kTemp: keep = number >= manifest; break;
      case FileType::kCurrent:
      case FileType::kLock:
      case FileType::kUnknown: keep = true; break;
    }
    if (keep) continue;
    if (type == FileType::kTable) table_cache_->Evict(number);
    (void)RemoveFile(dbname_ + "/" + n);
  }
}

DBStats DBImpl::GetStats() const {
  DBStats st;
  st.user_bytes_written = user_bytes_.load();
  st.wal_bytes_written = wal_bytes_.load();
  st.flush_bytes_written = flush_bytes_.load();
  st.compaction_bytes_written = compaction_bytes_written_.load();
  st.compaction_bytes_read = compaction_bytes_read_.load();
  st.group_commits = queue_->groups();
  st.group_commit_writers = queue_->grouped_writers();
  st.block_cache_hits = block_cache_->hits();
  st.block_cache_misses = block_cache_->misses();
  st.bloom_useful = table_stats_.bloom_useful.load();
  st.io_bytes_written = io_->stats().bytes_written.load();
  st.io_bytes_read = io_->stats().bytes_read.load();
  st.write_stalls = write_stalls_.load();
  auto v = versions_->current();
  for (int level = 0; level < v->num_levels(); ++level) {
    st.files_per_level.push_back(v->NumFiles(level));
    st.bytes_per_level.push_back(v->LevelBytes(level));
  }
  st.io_backend = io_->Name();
  return st;
}

}  // namespace lsm
