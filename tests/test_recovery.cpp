#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <optional>
#include <random>
#include <thread>

#include "lsm/db.hpp"
#include "test_util.hpp"
#include "util/file_util.hpp"

namespace lsm {
namespace {

using test::TempDir;

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "user%06d", i);
  return buf;
}
std::string Value(int i, int round, size_t len = 120) {
  std::string v = "r" + std::to_string(round) + "-" + std::to_string(i) + "-";
  while (v.size() < len) v.push_back(static_cast<char>('a' + (i + round + v.size()) % 26));
  return v;
}

// Wraps a real backend and fails every Sync() once `fail_after` syncs have
// succeeded — the in-process analogue of crash/eio_shim.c.
class FaultyBackend final : public IoBackend {
 public:
  FaultyBackend(std::shared_ptr<IoBackend> inner, int fail_after) : inner_(std::move(inner)), left_(fail_after) {}
  [[nodiscard]] std::string Name() const override { return "faulty(" + inner_->Name() + ")"; }
  void RegisterFile(int fd) override { inner_->RegisterFile(fd); }
  void UnregisterFile(int fd) override { inner_->UnregisterFile(fd); }
  Status Submit(std::span<IoRequest> reqs) override { return inner_->Submit(reqs); }
  AlignedBuffer AllocateBuffer(size_t c) override { return inner_->AllocateBuffer(c); }
  Status Sync(int fd, SyncMode mode) override {
    if (left_.fetch_sub(1) <= 0) {
      syncs_failed_++;
      return Status::IOError("fdatasync (injected)", EIO);
    }
    return inner_->Sync(fd, mode);
  }
  std::atomic<int> syncs_failed_{0};

 private:
  std::shared_ptr<IoBackend> inner_;
  std::atomic<int> left_;
};

class DBTest : public ::testing::TestWithParam<IoBackendKind> {
 protected:
  void SetUp() override {
    if (!test::MakeBackend(GetParam())) GTEST_SKIP() << "backend unavailable";
    options_.io_backend = GetParam();
    options_.write_buffer_size = 256 * 1024;
    options_.target_file_size = 128 * 1024;
    options_.max_bytes_for_level_base = 512 * 1024;
    options_.level0_compaction_trigger = 2;
    options_.wal_preallocate_bytes = 512 * 1024;
    options_.block_cache_capacity = 4 << 20;
    options_.abort_on_fsync_error = false;  // Observe panics in-process.
  }

  void Open() {
    db_.reset();
    ASSERT_TRUE(DB::Open(options_, dir_.path(), &db_).ok());
  }

  std::optional<std::string> Get(const std::string& k) {
    std::string v;
    Status s = db_->Get(ReadOptions(), k, &v);
    if (s.IsNotFound()) return std::nullopt;
    EXPECT_TRUE(s.ok()) << s.ToString();
    return v;
  }

  void CheckModel(const std::map<std::string, std::string>& model) {
    for (const auto& [k, v] : model) {
      auto got = Get(k);
      ASSERT_TRUE(got.has_value()) << k;
      ASSERT_EQ(*got, v) << k;
    }
    auto it = db_->NewIterator(ReadOptions());
    it->SeekToFirst();
    for (const auto& [k, v] : model) {
      ASSERT_TRUE(it->Valid()) << "iterator ended before " << k;
      ASSERT_EQ(it->key().ToString(), k);
      ASSERT_EQ(it->value().ToString(), v);
      it->Next();
    }
    EXPECT_FALSE(it->Valid()) << "ghost key " << it->key().ToString();
    EXPECT_TRUE(it->status().ok());
  }

  Options options_;
  TempDir dir_;
  std::unique_ptr<DB> db_;
};

TEST_P(DBTest, PutGetDeleteAndReopen) {
  Open();
  WriteOptions wo;
  ASSERT_TRUE(db_->Put(wo, "a", "1").ok());
  ASSERT_TRUE(db_->Put(wo, "b", "2").ok());
  ASSERT_TRUE(db_->Put(wo, "a", "3").ok());
  ASSERT_TRUE(db_->Delete(wo, "b").ok());
  EXPECT_EQ(Get("a"), "3");
  EXPECT_EQ(Get("b"), std::nullopt);
  EXPECT_EQ(Get("c"), std::nullopt);
  Open();  // WAL replay.
  EXPECT_EQ(Get("a"), "3");
  EXPECT_EQ(Get("b"), std::nullopt);
  Open();  // Replay of the L0 table produced by the previous recovery.
  EXPECT_EQ(Get("a"), "3");
  CheckModel({{"a", "3"}});
}

TEST_P(DBTest, RejectsOversizedKeys) {
  Open();
  EXPECT_TRUE(db_->Put(WriteOptions(), std::string(70000, 'k'), "v").IsInvalidArgument());
}

TEST_P(DBTest, FlushCompactionAndRecoveryPreserveEverything) {
  Open();
  std::map<std::string, std::string> model;
  std::mt19937 rng(42);
  WriteOptions wo;
  wo.sync = false;
  for (int round = 0; round < 4; ++round) {
    for (int i = 0; i < 6000; ++i) {
      const int k = static_cast<int>(rng() % 8000);
      if (rng() % 10 == 0) {
        ASSERT_TRUE(db_->Delete(wo, Key(k)).ok());
        model.erase(Key(k));
      } else {
        ASSERT_TRUE(db_->Put(wo, Key(k), Value(k, round)).ok());
        model[Key(k)] = Value(k, round);
      }
    }
  }
  ASSERT_TRUE(db_->WaitForCompactions().ok());
  DBStats st = db_->GetStats();
  int deep_files = 0;
  for (size_t l = 1; l < st.files_per_level.size(); ++l) deep_files += st.files_per_level[l];
  EXPECT_GT(deep_files, 0) << "expected leveled compaction to populate L1+";
  CheckModel(model);

  Open();
  CheckModel(model);
  ASSERT_TRUE(db_->CompactAll().ok());
  CheckModel(model);
  st = db_->GetStats();
  for (int l = 0; l + 1 < options_.num_levels; ++l) EXPECT_EQ(st.files_per_level[static_cast<size_t>(l)], 0);
  Open();
  CheckModel(model);

  // Only the live manifest survives.
  std::vector<std::string> names;
  ASSERT_TRUE(ListDir(dir_.path(), &names).ok());
  int manifests = 0;
  for (const auto& n : names) manifests += n.rfind("MANIFEST-", 0) == 0;
  EXPECT_EQ(manifests, 1);
}

TEST_P(DBTest, ConcurrentSyncWritersAreGroupCommitted) {
  Open();
  constexpr int kThreads = 8, kPer = 300;
  std::vector<std::thread> th;
  std::atomic<int> failures{0};
  for (int t = 0; t < kThreads; ++t) {
    th.emplace_back([&, t] {
      WriteOptions wo;
      wo.sync = true;
      for (int i = 0; i < kPer; ++i) {
        if (!db_->Put(wo, Key(t * kPer + i), Value(i, t)).ok()) failures++;
      }
    });
  }
  for (auto& x : th) x.join();
  ASSERT_EQ(failures.load(), 0);
  const DBStats st = db_->GetStats();
  EXPECT_EQ(st.group_commit_writers, static_cast<uint64_t>(kThreads * kPer));
  EXPECT_LT(st.group_commits, st.group_commit_writers);
  std::printf("backend %s: %llu writers / %llu fdatasync groups\n", st.io_backend.c_str(),
              static_cast<unsigned long long>(st.group_commit_writers),
              static_cast<unsigned long long>(st.group_commits));

  std::map<std::string, std::string> model;
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kPer; ++i) model[Key(t * kPer + i)] = Value(i, t);
  }
  Open();
  CheckModel(model);
}

TEST_P(DBTest, ConcurrentReadersDuringWritesAndFlushes) {
  Open();
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};
  // Writers only ever increase a key's version, so a reader must never see a
  // version go backwards.
  std::thread writer([&] {
    WriteOptions wo;
    wo.sync = false;
    for (int round = 1; round <= 40 && !stop; ++round) {
      for (int k = 0; k < 500; ++k) (void)db_->Put(wo, Key(k), std::to_string(round) + std::string(200, 'p'));
    }
    stop = true;
  });
  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&] {
      std::vector<int> last(500, 0);
      while (!stop) {
        for (int k = 0; k < 500; k += 7) {
          std::string v;
          Status s = db_->Get(ReadOptions(), Key(k), &v);
          if (s.IsNotFound()) {
            if (last[static_cast<size_t>(k)] != 0) bad++;
            continue;
          }
          const int ver = std::stoi(v);
          if (ver < last[static_cast<size_t>(k)]) bad++;
          last[static_cast<size_t>(k)] = ver;
        }
      }
    });
  }
  writer.join();
  for (auto& r : readers) r.join();
  EXPECT_EQ(bad.load(), 0);
}

TEST_P(DBTest, FsyncFailurePanicsAndRecoveryKeepsAcknowledgedWrites) {
  auto inner = test::MakeBackend(GetParam());
  // Open needs a handful of syncs (WAL dir, manifest, CURRENT, dir); then
  // allow some commits through before the injected EIO.
  auto faulty = std::make_shared<FaultyBackend>(inner, 30);
  Options o = options_;
  o.custom_backend = faulty;
  {
    std::unique_ptr<DB> db;
    ASSERT_TRUE(DB::Open(o, dir_.path(), &db).ok());
    std::map<std::string, std::string> acked;
    WriteOptions wo;
    wo.sync = true;
    Status s;
    int i = 0;
    for (; i < 10000; ++i) {
      s = db->Put(wo, Key(i), Value(i, 0));
      if (!s.ok()) break;
      acked[Key(i)] = Value(i, 0);
    }
    ASSERT_TRUE(s.IsPanic()) << s.ToString();
    EXPECT_GT(faulty->syncs_failed_.load(), 0);
    EXPECT_TRUE(db->BackgroundError().IsPanic());
    // Read-only from now on: every mutation is refused, no fsync is retried.
    const int failed_before = faulty->syncs_failed_.load();
    EXPECT_TRUE(db->Put(wo, "after", "x").IsPanic());
    EXPECT_TRUE(db->Flush().IsPanic());
    EXPECT_EQ(faulty->syncs_failed_.load(), failed_before);
    std::string v;
    EXPECT_TRUE(db->Get(ReadOptions(), Key(0), &v).ok());  // Reads still work.
    db.reset();

    // Restart on a healthy backend: everything acknowledged is there, the
    // write whose fdatasync failed may or may not be, nothing else is.
    Open();
    for (const auto& [k, val] : acked) {
      auto got = Get(k);
      ASSERT_TRUE(got.has_value()) << k;
      ASSERT_EQ(*got, val);
    }
    auto it = db_->NewIterator(ReadOptions());
    int extra = 0;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      if (!acked.contains(it->key().ToString())) {
        EXPECT_EQ(it->key().ToString(), Key(i)) << "ghost key";
        ++extra;
      }
    }
    EXPECT_LE(extra, 1);
    EXPECT_FALSE(Get("after").has_value());
  }
}

TEST_P(DBTest, DatabaseLockIsExclusive) {
  Open();
  std::unique_ptr<DB> second;
  EXPECT_TRUE(DB::Open(options_, dir_.path(), &second).IsBusy());
}

TEST_P(DBTest, MissingDatabaseWithoutCreateFails) {
  options_.create_if_missing = false;
  std::unique_ptr<DB> db;
  EXPECT_TRUE(DB::Open(options_, dir_.Sub("nope"), &db).IsInvalidArgument());
}

TEST_P(DBTest, CorruptCurrentIsReported) {
  Open();
  db_.reset();
  ASSERT_TRUE(WriteFileSync(CurrentFileName(dir_.path()), "garbage").ok());
  std::unique_ptr<DB> db;
  EXPECT_TRUE(DB::Open(options_, dir_.path(), &db).IsCorruption());
}

INSTANTIATE_TEST_SUITE_P(Backends, DBTest, ::testing::Values(IoBackendKind::kPread, IoBackendKind::kUring),
                         [](const auto& p) { return test::BackendName(p.param); });

}  // namespace
}  // namespace lsm
