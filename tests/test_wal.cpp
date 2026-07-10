#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "test_util.hpp"
#include "util/coding.hpp"
#include "util/crc32c.hpp"
#include "util/file_util.hpp"
#include "wal/log_reader.hpp"
#include "wal/log_writer.hpp"

namespace lsm {
namespace {

using test::TempDir;

// ---------------------------------------------------------------------------
// CRC32C

TEST(Crc32c, KnownVectors) {
  // RFC 3720 B.4 test vectors.
  char zeros[32] = {};
  EXPECT_EQ(crc32c::Value(zeros, sizeof(zeros)), 0x8a9136aau);
  char ones[32];
  std::memset(ones, 0xff, sizeof(ones));
  EXPECT_EQ(crc32c::Value(ones, sizeof(ones)), 0x62a8ab43u);
  char inc[32];
  for (int i = 0; i < 32; ++i) inc[i] = static_cast<char>(i);
  EXPECT_EQ(crc32c::Value(inc, sizeof(inc)), 0x46dd794eu);
  EXPECT_EQ(crc32c::Value("123456789", 9), 0xe3069283u);
}

TEST(Crc32c, ExtendMatchesOneShotForEverySplitAndAlignment) {
  std::string data(1000, '\0');
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i * 7 + 3);
  for (size_t off = 0; off < 9; ++off) {
    const uint32_t whole = crc32c::Value(data.data() + off, data.size() - off);
    for (size_t split = 0; split < 64; ++split) {
      const uint32_t a = crc32c::Value(data.data() + off, split);
      EXPECT_EQ(crc32c::Extend(a, data.data() + off + split, data.size() - off - split), whole);
    }
  }
}

TEST(Crc32c, MaskRoundTrip) {
  const uint32_t c = crc32c::Value("foo", 3);
  EXPECT_NE(crc32c::Mask(c), c);
  EXPECT_EQ(crc32c::Unmask(crc32c::Mask(c)), c);
  std::printf("crc32c hardware accelerated: %s\n", crc32c::HardwareAccelerated() ? "yes" : "no");
}

// ---------------------------------------------------------------------------
// IoBackend

class BackendTest : public ::testing::TestWithParam<IoBackendKind> {
 protected:
  void SetUp() override {
    io_ = test::MakeBackend(GetParam());
    if (!io_) GTEST_SKIP() << test::BackendName(GetParam()) << " backend unavailable here";
  }
  std::shared_ptr<IoBackend> io_;
  TempDir dir_;
};

TEST_P(BackendTest, AlignedWriteReadRoundTrip) {
  int fd = -1;
  bool direct = false;
  ASSERT_TRUE(OpenFile(dir_.Sub("f"), O_RDWR | O_CREAT, 0644, true, true, &fd, &direct).ok());
  io_->RegisterFile(fd);
  AlignedBuffer w = io_->AllocateBuffer(64 * 1024);
  for (size_t i = 0; i < 64 * 1024; ++i) w.data()[i] = static_cast<char>(i % 251);
  ASSERT_TRUE(io_->Write(fd, w.data(), 64 * 1024, 0, w.fixed_index()).ok());
  ASSERT_TRUE(io_->Sync(fd, SyncMode::kData).ok());

  // Batched reads of every 4 KiB page, in reverse order.
  std::vector<AlignedBuffer> bufs;
  std::vector<IoRequest> reqs;
  for (int i = 15; i >= 0; --i) {
    bufs.push_back(AlignedBuffer::Allocate(kIoAlignment));
    IoRequest r;
    r.op = IoOpcode::kRead;
    r.fd = fd;
    r.buf = bufs.back().data();
    r.len = kIoAlignment;
    r.offset = static_cast<uint64_t>(i) * kIoAlignment;
    reqs.push_back(r);
  }
  ASSERT_TRUE(io_->Submit(reqs).ok());
  for (size_t j = 0; j < reqs.size(); ++j) {
    ASSERT_EQ(reqs[j].result, static_cast<int64_t>(kIoAlignment));
    ASSERT_EQ(std::memcmp(bufs[j].data(), w.data() + reqs[j].offset, kIoAlignment), 0);
  }

  // Short read at EOF.
  AlignedBuffer big = AlignedBuffer::Allocate(128 * 1024);
  size_t got = 0;
  ASSERT_TRUE(io_->Read(fd, big.data(), 128 * 1024, 0, &got).ok());
  EXPECT_EQ(got, 64u * 1024);
  ASSERT_TRUE(io_->Read(fd, big.data(), kIoAlignment, 1 << 20, &got).ok());
  EXPECT_EQ(got, 0u);
  io_->UnregisterFile(fd);
  ::close(fd);
  std::printf("backend=%s direct=%d\n", io_->Name().c_str(), direct);
}

TEST_P(BackendTest, MisalignedRequestsAreRejected) {
  int fd = -1;
  ASSERT_TRUE(OpenFile(dir_.Sub("f"), O_RDWR | O_CREAT, 0644, true, true, &fd, nullptr).ok());
  AlignedBuffer b = AlignedBuffer::Allocate(2 * kIoAlignment);
  EXPECT_TRUE(io_->Write(fd, b.data() + 1, kIoAlignment, 0).IsInvalidArgument());
  EXPECT_TRUE(io_->Write(fd, b.data(), kIoAlignment, 100).IsInvalidArgument());
  EXPECT_TRUE(io_->Write(fd, b.data(), 100, 0).IsInvalidArgument());
  size_t n;
  EXPECT_TRUE(io_->Read(fd, b.data(), 100, 0, &n).IsInvalidArgument());
  ::close(fd);
}

TEST_P(BackendTest, ConcurrentSubmittersShareRings) {
  int fd = -1;
  ASSERT_TRUE(OpenFile(dir_.Sub("f"), O_RDWR | O_CREAT, 0644, true, true, &fd, nullptr).ok());
  io_->RegisterFile(fd);
  constexpr int kThreads = 8, kPages = 64;
  std::vector<std::thread> th;
  std::atomic<int> failures{0};
  for (int t = 0; t < kThreads; ++t) {
    th.emplace_back([&, t] {
      AlignedBuffer b = AlignedBuffer::Allocate(kIoAlignment);
      for (int p = 0; p < kPages; ++p) {
        std::memset(b.data(), t * kPages + p, kIoAlignment);
        const uint64_t off = static_cast<uint64_t>(t * kPages + p) * kIoAlignment;
        if (!io_->Write(fd, b.data(), kIoAlignment, off).ok()) failures++;
      }
      for (int p = 0; p < kPages; ++p) {
        size_t got = 0;
        const uint64_t off = static_cast<uint64_t>(t * kPages + p) * kIoAlignment;
        if (!io_->Read(fd, b.data(), kIoAlignment, off, &got).ok() || got != kIoAlignment ||
            static_cast<unsigned char>(b.data()[100]) != static_cast<unsigned char>(t * kPages + p)) {
          failures++;
        }
      }
    });
  }
  for (auto& x : th) x.join();
  EXPECT_EQ(failures.load(), 0);
  io_->UnregisterFile(fd);
  ::close(fd);
}

INSTANTIATE_TEST_SUITE_P(Backends, BackendTest, ::testing::Values(IoBackendKind::kPread, IoBackendKind::kUring),
                         [](const auto& p) { return test::BackendName(p.param); });

// ---------------------------------------------------------------------------
// LogWriter / LogReader

struct WalParam {
  IoBackendKind backend;
  bool pad_commits;
};

class WalTest : public ::testing::TestWithParam<WalParam> {
 protected:
  void SetUp() override {
    options_.io_backend = GetParam().backend;
    options_.wal_pad_commits = GetParam().pad_commits;
    options_.wal_preallocate_bytes = 1 << 20;
    io_ = test::MakeBackend(GetParam().backend);
    if (!io_) GTEST_SKIP() << "backend unavailable";
    path_ = dir_.Sub("000001.log");
  }

  std::vector<std::pair<uint64_t, std::vector<WriteBatch::Op>>> ReadAll(uint64_t* good = nullptr,
                                                                        std::string* reason = nullptr) {
    std::unique_ptr<wal::LogReader> r;
    EXPECT_TRUE(wal::LogReader::Open(io_.get(), path_, options_, &r).ok());
    std::vector<std::pair<uint64_t, std::vector<WriteBatch::Op>>> out;
    WriteBatch b;
    for (;;) {
      uint64_t seq = 0;
      bool eof = false;
      EXPECT_TRUE(r->ReadBatch(&b, &seq, &eof).ok());
      if (eof) break;
      out.emplace_back(seq, b.ops());
    }
    if (good) *good = r->good_offset();
    if (reason) *reason = r->tail_reason();
    return out;
  }

  Options options_;
  std::shared_ptr<IoBackend> io_;
  TempDir dir_;
  std::string path_;
};

TEST_P(WalTest, GroupsRoundTripWithSequenceNumbers) {
  std::unique_ptr<wal::LogWriter> w;
  ASSERT_TRUE(wal::LogWriter::Create(io_.get(), path_, 1, options_, &w).ok());
  std::vector<WriteBatch> batches(300);
  uint64_t seq = 100;
  std::vector<uint64_t> first_seqs;
  for (size_t g = 0; g < 100; ++g) {
    std::vector<wal::LogWriter::BatchRef> refs;
    for (size_t j = 0; j < 3; ++j) {
      WriteBatch& b = batches[g * 3 + j];
      const size_t n = 1 + (g + j) % 4;
      for (size_t k = 0; k < n; ++k) {
        const std::string key = "k" + std::to_string(g) + "_" + std::to_string(j) + "_" + std::to_string(k);
        if ((g + k) % 5 == 0) b.Delete(key);
        else b.Put(key, std::string(1 + (g * 37 + k * 11) % 3000, static_cast<char>('a' + g % 26)));
      }
      refs.push_back({&b, seq});
      first_seqs.push_back(seq);
      seq += b.Count();
    }
    ASSERT_TRUE(w->AddGroup(refs, g % 3 == 0).ok());
  }
  ASSERT_TRUE(w->Close().ok());

  auto got = ReadAll();
  ASSERT_EQ(got.size(), batches.size());
  for (size_t i = 0; i < got.size(); ++i) {
    EXPECT_EQ(got[i].first, first_seqs[i]);
    ASSERT_EQ(got[i].second.size(), batches[i].ops().size());
    for (size_t k = 0; k < got[i].second.size(); ++k) {
      EXPECT_EQ(got[i].second[k].type, batches[i].ops()[k].type);
      EXPECT_EQ(got[i].second[k].key, batches[i].ops()[k].key);
      EXPECT_EQ(got[i].second[k].value, batches[i].ops()[k].value);
    }
  }
}

TEST_P(WalTest, RecordsSpanningBlocksAndHugeValues) {
  std::unique_ptr<wal::LogWriter> w;
  ASSERT_TRUE(wal::LogWriter::Create(io_.get(), path_, 1, options_, &w).ok());
  WriteBatch b1, b2;
  b1.Put("big", std::string(3 << 20, 'x'));  // Larger than the staging buffer.
  b2.Put("small", "y");
  wal::LogWriter::BatchRef r1{&b1, 1}, r2{&b2, 2};
  ASSERT_TRUE(w->AddGroup({&r1, 1}, true).ok());
  ASSERT_TRUE(w->AddGroup({&r2, 1}, true).ok());
  ASSERT_TRUE(w->Close().ok());
  auto got = ReadAll();
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].second[0].value.size(), 3u << 20);
  EXPECT_EQ(got[1].second[0].key, "small");
}

TEST_P(WalTest, PaddingModeAlignsEveryCommit) {
  if (!GetParam().pad_commits) GTEST_SKIP();
  std::unique_ptr<wal::LogWriter> w;
  ASSERT_TRUE(wal::LogWriter::Create(io_.get(), path_, 1, options_, &w).ok());
  WriteBatch b;
  b.Put("a", "b");
  for (uint64_t i = 1; i <= 5; ++i) {
    wal::LogWriter::BatchRef r{&b, i};
    ASSERT_TRUE(w->AddGroup({&r, 1}, true).ok());
    EXPECT_EQ(w->logical_size() % kIoAlignment, 0u);
    EXPECT_EQ(w->logical_size(), i * kIoAlignment);
  }
  ASSERT_TRUE(w->Close().ok());
  EXPECT_EQ(ReadAll().size(), 5u);
}

void CorruptAt(const std::string& path, uint64_t off, const std::string& bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(off)), static_cast<ssize_t>(bytes.size()));
  ::close(fd);
}

TEST_P(WalTest, TornTailStopsAtLastCompleteBatch) {
  std::unique_ptr<wal::LogWriter> w;
  ASSERT_TRUE(wal::LogWriter::Create(io_.get(), path_, 1, options_, &w).ok());
  std::vector<WriteBatch> batches(10);
  std::vector<uint64_t> ends;
  for (uint64_t i = 0; i < batches.size(); ++i) {
    for (int k = 0; k < 4; ++k) batches[i].Put("key" + std::to_string(i * 4 + k), std::string(700, 'v'));
    wal::LogWriter::BatchRef r{&batches[i], i * 4 + 1};
    ASSERT_TRUE(w->AddGroup({&r, 1}, true).ok());
    ends.push_back(w->logical_size());
  }
  ASSERT_TRUE(w->Close().ok());

  // Flip a byte in the middle of batch 7's records: batches 0..6 survive.
  const uint64_t mid = ends[6] + (ends[7] - ends[6]) / 2;
  CorruptAt(path_, mid, "\x5a");
  uint64_t good = 0;
  std::string reason;
  auto got = ReadAll(&good, &reason);
  ASSERT_EQ(got.size(), 7u);
  EXPECT_FALSE(reason.empty());
  EXPECT_LE(good, mid);
  EXPECT_EQ(got.back().first, 6u * 4 + 1);
}

TEST_P(WalTest, ZeroedTailOfBatchDropsWholeBatch) {
  std::unique_ptr<wal::LogWriter> w;
  ASSERT_TRUE(wal::LogWriter::Create(io_.get(), path_, 1, options_, &w).ok());
  WriteBatch a, b;
  a.Put("a", "1");
  for (int k = 0; k < 50; ++k) b.Put("b" + std::to_string(k), std::string(200, 'z'));
  wal::LogWriter::BatchRef ra{&a, 1}, rb{&b, 2};
  ASSERT_TRUE(w->AddGroup({&ra, 1}, true).ok());
  const uint64_t after_a = w->logical_size();
  ASSERT_TRUE(w->AddGroup({&rb, 1}, true).ok());
  const uint64_t end = w->logical_size();
  ASSERT_TRUE(w->Close().ok());
  // Simulate an unflushed (never persisted) second half of batch b.
  const uint64_t cut = after_a + (end - after_a) / 2;
  CorruptAt(path_, cut, std::string(end - cut, '\0'));
  auto got = ReadAll();
  ASSERT_EQ(got.size(), 1u);  // Batch atomicity: no partial b.
  EXPECT_EQ(got[0].second[0].key, "a");
}

INSTANTIATE_TEST_SUITE_P(Wal, WalTest,
                         ::testing::Values(WalParam{IoBackendKind::kPread, false}, WalParam{IoBackendKind::kPread, true},
                                           WalParam{IoBackendKind::kUring, false}, WalParam{IoBackendKind::kUring, true}),
                         [](const auto& p) {
                           return std::string(test::BackendName(p.param.backend)) +
                                  (p.param.pad_commits ? "_Padded" : "_TailRewrite");
                         });

// ---------------------------------------------------------------------------
// Group commit

TEST(GroupCommit, SequencesAreDenseOrderedAndBatched) {
  std::mutex mu;
  std::vector<std::pair<uint64_t, uint64_t>> logged;  // [first, last] per group
  std::atomic<int> concurrent_logs{0};
  std::atomic<bool> overlap{false};
  std::atomic<uint64_t> applied{0};

  wal::GroupCommitQueue* qp = nullptr;
  wal::GroupCommitQueue q(
      [&](wal::GroupCommitQueue::Group& g) {
        if (concurrent_logs.fetch_add(1) != 0) overlap = true;
        std::this_thread::sleep_for(std::chrono::microseconds(200));  // Pretend fdatasync.
        {
          std::lock_guard lk(mu);
          logged.emplace_back(g.first_seq, g.last_seq);
        }
        concurrent_logs.fetch_sub(1);
        return Status::OK();
      },
      [&](wal::GroupCommitQueue::Writer& w, wal::GroupCommitQueue::Group&) {
        // Our own sequence range must not be visible yet.
        if (qp->LastPublished() >= w.first_seq) overlap = true;
        applied.fetch_add(w.batch->Count());
        return Status::OK();
      },
      1 << 20);
  qp = &q;
  q.SetLastSequence(1000);

  constexpr int kThreads = 16, kPer = 400;
  std::vector<std::thread> th;
  std::mutex seen_mu;
  std::set<uint64_t> seen;
  for (int t = 0; t < kThreads; ++t) {
    th.emplace_back([&, t] {
      WriteBatch b;
      for (int k = 0; k <= t % 3; ++k) b.Put("k", "v");
      for (int i = 0; i < kPer; ++i) {
        wal::GroupCommitQueue::Writer w;
        w.batch = &b;
        w.sync = true;
        ASSERT_TRUE(q.Commit(&w).ok());
        // Read-your-writes: visible once Commit returns.
        ASSERT_GE(q.LastPublished(), w.first_seq + b.Count() - 1);
        std::lock_guard lk(seen_mu);
        for (uint64_t s = w.first_seq; s < w.first_seq + b.Count(); ++s) ASSERT_TRUE(seen.insert(s).second);
      }
    });
  }
  for (auto& x : th) x.join();

  EXPECT_FALSE(overlap.load()) << "log callbacks overlapped or a range was published early";
  const uint64_t total = applied.load();
  EXPECT_EQ(q.LastPublished(), 1000 + total);
  ASSERT_EQ(seen.size(), total);
  EXPECT_EQ(*seen.begin(), 1001u);
  EXPECT_EQ(*seen.rbegin(), 1000 + total);
  // Groups were logged in sequence order with no gaps.
  uint64_t next = 1001;
  for (auto [first, last] : logged) {
    EXPECT_EQ(first, next);
    next = last + 1;
  }
  EXPECT_LT(q.groups(), static_cast<uint64_t>(kThreads * kPer)) << "no batching happened";
  std::printf("group commit: %llu writers in %llu groups (%.1f per group)\n",
              static_cast<unsigned long long>(q.grouped_writers()), static_cast<unsigned long long>(q.groups()),
              static_cast<double>(q.grouped_writers()) / static_cast<double>(q.groups()));
}

TEST(GroupCommit, FailedLogRejectsWholeGroupAndRollsBackSequences) {
  std::atomic<int> calls{0};
  wal::GroupCommitQueue q(
      [&](wal::GroupCommitQueue::Group&) {
        return calls.fetch_add(1) == 0 ? Status::IOError("fdatasync", EIO) : Status::OK();
      },
      [&](wal::GroupCommitQueue::Writer&, wal::GroupCommitQueue::Group&) { return Status::OK(); }, 1 << 20);
  q.SetLastSequence(0);
  WriteBatch b;
  b.Put("a", "b");
  wal::GroupCommitQueue::Writer w1;
  w1.batch = &b;
  EXPECT_TRUE(q.Commit(&w1).IsIOError());
  EXPECT_EQ(q.LastAllocated(), 0u);
  wal::GroupCommitQueue::Writer w2;
  w2.batch = &b;
  EXPECT_TRUE(q.Commit(&w2).ok());
  EXPECT_EQ(w2.first_seq, 1u);
  EXPECT_EQ(q.LastPublished(), 1u);
}

}  // namespace
}  // namespace lsm
