#include "wal/log_writer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <thread>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#include "util/coding.hpp"
#include "util/crc32c.hpp"
#include "util/file_util.hpp"

namespace lsm::wal {

namespace {
alignas(kIoAlignment) const char kZeros[kBlockSize] = {};

inline void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}

std::string DirName(const std::string& path) {
  const auto pos = path.rfind('/');
  if (pos == std::string::npos) return ".";
  if (pos == 0) return "/";
  return path.substr(0, pos);
}
}  // namespace

// ---------------------------------------------------------------------------
// LogWriter

LogWriter::LogWriter(IoBackend* io, int fd, bool direct, uint64_t log_number, const Options& options)
    : io_(io),
      fd_(fd),
      direct_(direct),
      log_number_(log_number),
      pad_commits_(options.wal_pad_commits),
      defer_unsynced_(options.wal_defer_unsynced_writes),
      buf_(io->AllocateBuffer(std::max<size_t>(256 * 1024, AlignUp(options.wal_max_group_bytes / 4)))) {}

LogWriter::~LogWriter() {
  if (fd_ >= 0) Abandon();
}

Status LogWriter::Create(IoBackend* io, const std::string& path, uint64_t log_number, const Options& options,
                         std::unique_ptr<LogWriter>* out) {
  int fd = -1;
  bool direct = false;
  LSM_RETURN_IF_ERROR(OpenFile(path, O_WRONLY | O_CREAT | O_EXCL, 0644, options.use_direct_io,
                               options.allow_buffered_fallback, &fd, &direct));
  if (options.wal_preallocate_bytes > 0) {
    // Allocating up front means the per-commit fdatasync does not also have
    // to journal block allocations. Unwritten space reads as zeros, which
    // the reader treats as end-of-log. Unsupported filesystems are fine.
    (void)::fallocate(fd, 0, 0, static_cast<off_t>(AlignUp(options.wal_preallocate_bytes)));
  }
  // The directory entry must be durable before any record in this file is
  // acknowledged, or a crash could drop the whole file.
  Status s = FsyncDir(DirName(path));
  if (!s.ok()) {
    ::close(fd);
    return s;
  }
  io->RegisterFile(fd);
  out->reset(new LogWriter(io, fd, direct, log_number, options));
  return Status::OK();
}

void LogWriter::EncodeRecord(RecordType type, const Slice& key, const Slice& value, std::string* dst) {
  char header[kHeaderSize];
  EncodeFixed32(header + 4, static_cast<uint32_t>(key.size() + value.size()));
  header[8] = static_cast<char>(type);
  EncodeFixed16(header + 9, static_cast<uint16_t>(key.size()));
  uint32_t crc = crc32c::Extend(0, header + 4, kHeaderSize - 4);
  crc = crc32c::Extend(crc, key.data(), key.size());
  crc = crc32c::Extend(crc, value.data(), value.size());
  EncodeFixed32(header, crc32c::Mask(crc));
  dst->append(header, kHeaderSize);
  dst->append(key.data(), key.size());
  dst->append(value.data(), value.size());
}

void LogWriter::EnsureCapacity(size_t extra) {
  // + one block for boundary filler / commit padding.
  const size_t need = buf_len_ + extra + kBlockSize;
  if (need <= buf_.capacity()) return;
  AlignedBuffer bigger = io_->AllocateBuffer(AlignUp(std::max(need, buf_.capacity() * 2)));
  std::memcpy(bigger.data(), buf_.data(), buf_len_);
  buf_ = std::move(bigger);
}

void LogWriter::AppendBytes(const char* p, size_t n) {
  std::memcpy(buf_.data() + buf_len_, p, n);
  buf_len_ += n;
}

void LogWriter::AppendRecord(RecordType type, const Slice& key, const Slice& value) {
  const size_t in_block = buf_len_ % kBlockSize;  // buf_[0] is block aligned.
  if (kBlockSize - in_block < kHeaderSize) AppendBytes(kZeros, kBlockSize - in_block);
  char* header = buf_.data() + buf_len_;
  EncodeFixed32(header + 4, static_cast<uint32_t>(key.size() + value.size()));
  header[8] = static_cast<char>(type);
  EncodeFixed16(header + 9, static_cast<uint16_t>(key.size()));
  buf_len_ += kHeaderSize;
  AppendBytes(key.data(), key.size());
  AppendBytes(value.data(), value.size());
  // Checksum the bytes in place: header tail, key and value are contiguous.
  const uint32_t crc = crc32c::Value(header + 4, kHeaderSize - 4 + key.size() + value.size());
  EncodeFixed32(header, crc32c::Mask(crc));
}

void LogWriter::PadToBlockBoundary() {
  const size_t in_block = buf_len_ % kBlockSize;
  if (in_block == 0) return;
  const size_t remaining = kBlockSize - in_block;
  if (remaining < kHeaderSize) {
    AppendBytes(kZeros, remaining);
  } else {
    AppendRecord(RecordType::kPadding, Slice(), Slice(kZeros, remaining - kHeaderSize));
  }
}

Status LogWriter::FlushBuffer() {
  if (fd_ < 0) return Status::IOError("write to closed WAL", EBADF);
  const size_t len = buf_len_;
  if (len == 0) return Status::OK();
  const size_t write_len = AlignUp(len);
  std::memset(buf_.data() + len, 0, write_len - len);
  Status s = io_->Write(fd_, buf_.data(), write_len, file_off_, buf_.fixed_index());
  if (!s.ok()) return s;
  bytes_submitted_ += write_len;
  dirty_ = true;
  // Keep the partially filled tail block; it is rewritten by the next commit.
  const size_t keep_from = AlignDown(len);
  const size_t tail = len - keep_from;
  if (tail > 0 && keep_from > 0) std::memmove(buf_.data(), buf_.data() + keep_from, tail);
  file_off_ += keep_from;
  buf_len_ = tail;
  unwritten_ = false;
  return Status::OK();
}

Status LogWriter::AddGroup(std::span<const BatchRef> batches, bool sync) {
  size_t estimate = 0;
  for (const auto& b : batches) {
    estimate += 2 * kHeaderSize + kBatchHeaderValueSize;
    for (const auto& op : b.batch->ops()) {
      if (op.key.size() > kMaxKeySize) return Status::InvalidArgument("key longer than 65535 bytes");
      if (op.key.size() + op.value.size() > 0xffffffffull - kHeaderSize) {
        return Status::InvalidArgument("record larger than 4 GiB");
      }
      // A record can be preceded by up to kHeaderSize-1 filler bytes.
      estimate += 2 * kHeaderSize + op.key.size() + op.value.size();
    }
  }
  EnsureCapacity(estimate);

  for (const auto& b : batches) {
    char hv[kBatchHeaderValueSize];
    EncodeFixed64(hv, b.first_seq);
    EncodeFixed32(hv + 8, static_cast<uint32_t>(b.batch->Count()));
    AppendRecord(RecordType::kBatchHeader, Slice(), Slice(hv, sizeof(hv)));
    for (const auto& op : b.batch->ops()) {
      AppendRecord(op.type == ValueType::kValue ? RecordType::kPut : RecordType::kDelete, op.key, op.value);
    }
  }
  unwritten_ = true;
  if (sync) return Sync();
  if (defer_unsynced_) return FlushFullBlocks();
  if (pad_commits_) PadToBlockBoundary();
  return FlushBuffer();
}

Status LogWriter::FlushFullBlocks() {
  const size_t full = AlignDown(buf_len_);
  if (full == 0) return Status::OK();
  if (fd_ < 0) return Status::IOError("write to closed WAL", EBADF);
  LSM_RETURN_IF_ERROR(io_->Write(fd_, buf_.data(), full, file_off_, buf_.fixed_index()));
  bytes_submitted_ += full;
  dirty_ = true;
  const size_t tail = buf_len_ - full;
  if (tail > 0) std::memmove(buf_.data(), buf_.data() + full, tail);
  file_off_ += full;
  buf_len_ = tail;
  unwritten_ = tail > 0;
  return Status::OK();
}

Status LogWriter::Sync() {
  if (fd_ < 0) return Status::IOError("sync of closed WAL", EBADF);
  if (unwritten_) {
    if (pad_commits_) PadToBlockBoundary();
    LSM_RETURN_IF_ERROR(FlushBuffer());
  }
  if (!dirty_) return Status::OK();
  Status s = io_->Sync(fd_, SyncMode::kData);
  if (s.ok()) dirty_ = false;
  return s;
}

Status LogWriter::Close() {
  if (fd_ < 0) return Status::OK();
  Status s = Sync();
  Abandon();  // Close regardless: a failed fd must never be synced again.
  return s;
}

void LogWriter::Abandon() noexcept {
  if (fd_ < 0) return;
  io_->UnregisterFile(fd_);
  ::close(fd_);
  fd_ = -1;
}

// ---------------------------------------------------------------------------
// GroupCommitQueue

GroupCommitQueue::GroupCommitQueue(LogFn log, ApplyFn apply, size_t max_group_bytes)
    : log_(std::move(log)), apply_(std::move(apply)), max_group_bytes_(max_group_bytes) {}

void GroupCommitQueue::SetLastSequence(uint64_t seq) noexcept {
  allocated_.store(seq, std::memory_order_release);
  published_.store(seq, std::memory_order_release);
}

void GroupCommitQueue::WaitForPublished(uint64_t seq) const noexcept {
  uint64_t cur;
  while ((cur = published_.load(std::memory_order_acquire)) < seq) published_.wait(cur, std::memory_order_acquire);
}

void GroupCommitQueue::Await(Writer* w, uint8_t mask) {
  for (int i = 0; i < 256; ++i) {
    if (w->state.load(std::memory_order_acquire) & mask) break;
    CpuRelax();
  }
  std::unique_lock lk(w->mu);
  // Even when the spin observed the transition we take the mutex once, so
  // the waker (which holds it while notifying) has let go before we return
  // and destroy w.
  w->cv.wait(lk, [&] { return (w->state.load(std::memory_order_acquire) & mask) != 0; });
}

void GroupCommitQueue::Wake(Writer* w, uint8_t state) {
  std::lock_guard lk(w->mu);
  w->state.store(state, std::memory_order_release);
  w->cv.notify_one();
}

Status GroupCommitQueue::Commit(Writer* w) {
  w->state.store(Writer::kInit, std::memory_order_relaxed);
  w->link_newer = nullptr;
  w->group = nullptr;
  w->status = Status::OK();

  Writer* head = newest_.load(std::memory_order_relaxed);
  do {
    w->link_older = head;
  } while (!newest_.compare_exchange_weak(head, w, std::memory_order_release, std::memory_order_relaxed));

  // The group lives in the leader's frame; the leader only returns once the
  // whole group has completed.
  std::optional<Group> group_storage;
  if (head == nullptr) {
    w->state.store(Writer::kLeader, std::memory_order_relaxed);
  } else {
    Await(w, Writer::kLeader | Writer::kLogged | Writer::kCompleted);
  }

  if (w->state.load(std::memory_order_acquire) == Writer::kLeader) {
    group_storage.emplace();
    w->group = &*group_storage;
    LeadGroup(w);
  }
  if (w->state.load(std::memory_order_acquire) == Writer::kLogged) {
    w->status = apply_(*w, *w->group);
    FinishApply(w);
  }
  return w->status;
}

void GroupCommitQueue::LeadGroup(Writer* leader) {
  Group& g = *leader->group;
  Writer* newest = newest_.load(std::memory_order_acquire);
  for (Writer* x = newest; x != leader; x = x->link_older) x->link_older->link_newer = x;

  size_t bytes = leader->batch->ApproximateBytes();
  g.writers.push_back(leader);
  Writer* last = leader;
  while (last != newest) {
    Writer* next = last->link_newer;
    const size_t b = next->batch->ApproximateBytes();
    if (bytes + b > max_group_bytes_) break;
    bytes += b;
    g.writers.push_back(next);
    last = next;
  }

  uint64_t seq = allocated_.load(std::memory_order_relaxed) + 1;
  g.first_seq = seq;
  for (Writer* x : g.writers) {
    x->first_seq = seq;
    x->group = &g;
    seq += x->batch->Count();
    g.sync |= x->sync;
  }
  g.last_seq = seq - 1;
  allocated_.store(g.last_seq, std::memory_order_release);

  Status s = log_(g);
  if (!s.ok()) allocated_.store(g.first_seq - 1, std::memory_order_release);  // Nothing was applied.
  groups_.fetch_add(1, std::memory_order_relaxed);
  grouped_writers_.fetch_add(g.writers.size(), std::memory_order_relaxed);

  // Hand leadership to the next queued writer so its WAL write overlaps with
  // this group's memtable inserts.
  Writer* expected = last;
  if (!newest_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    Writer* succ;
    if (last != newest) {
      succ = last->link_newer;
    } else {
      succ = newest_.load(std::memory_order_acquire);
      while (succ->link_older != last) succ = succ->link_older;
    }
    Wake(succ, Writer::kLeader);
  }

  if (!s.ok()) {
    for (Writer* x : g.writers) x->status = s;
    for (size_t i = 1; i < g.writers.size(); ++i) Wake(g.writers[i], Writer::kCompleted);
    leader->state.store(Writer::kCompleted, std::memory_order_release);
    return;
  }
  g.pending.store(g.writers.size(), std::memory_order_release);
  for (size_t i = 1; i < g.writers.size(); ++i) Wake(g.writers[i], Writer::kLogged);
  leader->state.store(Writer::kLogged, std::memory_order_release);
}

void GroupCommitQueue::FinishApply(Writer* self) {
  Group* g = self->group;
  if (g->pending.fetch_sub(1, std::memory_order_acq_rel) != 1) {
    Await(self, Writer::kCompleted);
    return;
  }
  // Last applier of the group: publish strictly in sequence order.
  const uint64_t expect = g->first_seq - 1;
  uint64_t cur;
  while ((cur = published_.load(std::memory_order_acquire)) != expect) {
    published_.wait(cur, std::memory_order_acquire);
  }
  published_.store(g->last_seq, std::memory_order_release);
  published_.notify_all();

  Writer* leader = g->writers[0];
  const size_t n = g->writers.size();
  for (size_t i = 1; i < n; ++i) {
    if (g->writers[i] != self) Wake(g->writers[i], Writer::kCompleted);
  }
  // The group is owned by the leader: wake it last and do not touch g after.
  if (self != leader) Wake(leader, Writer::kCompleted);
  else self->state.store(Writer::kCompleted, std::memory_order_release);
}

}  // namespace lsm::wal
