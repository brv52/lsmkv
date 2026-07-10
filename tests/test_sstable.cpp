#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <map>
#include <random>

#include "cache/lru_cache.hpp"
#include "sstable/block.hpp"
#include "sstable/block_builder.hpp"
#include "sstable/bloom.hpp"
#include "sstable/table_builder.hpp"
#include "sstable/table_reader.hpp"
#include "test_util.hpp"
#include "util/dbformat.hpp"
#include "util/file_util.hpp"

namespace lsm {
namespace {

using test::TempDir;

std::string Key(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%08d", i);
  return buf;
}

TEST(Block, PrefixCompressedRoundTripAndSeek) {
  sst::BlockBuilder bb(4);
  std::vector<std::string> keys;
  for (int i = 0; i < 200; ++i) keys.push_back(MakeInternalKey(Key(i * 2), 100, ValueType::kValue));
  for (size_t i = 0; i < keys.size(); ++i) bb.Add(keys[i], "v" + std::to_string(i));
  const Slice raw = bb.Finish();
  AlignedBuffer buf = AlignedBuffer::Allocate(raw.size());
  std::memcpy(buf.data(), raw.data(), raw.size());
  sst::Block block(std::move(buf), raw.size());
  ASSERT_TRUE(block.valid());

  auto it = block.NewIterator(sst::Block::KeyOrder::kInternal);
  it->SeekToFirst();
  for (size_t i = 0; i < keys.size(); ++i, it->Next()) {
    ASSERT_TRUE(it->Valid());
    EXPECT_EQ(it->key().ToString(), keys[i]);
    EXPECT_EQ(it->value().ToString(), "v" + std::to_string(i));
  }
  EXPECT_FALSE(it->Valid());

  it->Seek(MakeInternalKey(Key(51), 100, ValueType::kValue));  // Between key 50 and 52.
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(ExtractUserKey(it->key()).ToString(), Key(52));
  it->Seek(MakeInternalKey(Key(1000), 100, ValueType::kValue));
  EXPECT_FALSE(it->Valid());
  it->Seek(MakeInternalKey("", 100, ValueType::kValue));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(ExtractUserKey(it->key()).ToString(), Key(0));
}

TEST(Bloom, NoFalseNegativesAndAboutOnePercentFalsePositives) {
  sst::BloomFilterBuilder b(10);
  constexpr int kKeys = 100000;
  for (int i = 0; i < kKeys; ++i) b.AddHash(sst::BloomHash(Key(i)));
  const std::string filter = b.Finish();
  EXPECT_NEAR(static_cast<double>(filter.size()) * 8 / kKeys, 10.0, 0.1);
  for (int i = 0; i < kKeys; ++i) ASSERT_TRUE(sst::BloomFilterBuilder::MayContain(filter, sst::BloomHash(Key(i))));
  int fp = 0;
  for (int i = kKeys; i < 2 * kKeys; ++i) fp += sst::BloomFilterBuilder::MayContain(filter, sst::BloomHash(Key(i)));
  const double rate = static_cast<double>(fp) / kKeys;
  std::printf("bloom fp rate at 10 bits/key: %.3f%%\n", rate * 100);
  EXPECT_LT(rate, 0.015);
}

TEST(LruCache, EvictsLeastRecentlyUsedAndPinsByReference) {
  LruCache cache(4 * 100, 0);
  auto make = [](int v) { return std::shared_ptr<const void>(std::make_shared<const int>(v)); };
  for (int i = 0; i < 4; ++i) cache.Insert({1, static_cast<uint64_t>(i)}, make(i), 100);
  ASSERT_NE(cache.Lookup({1, 0}), nullptr);  // 0 becomes MRU.
  auto pinned = cache.Lookup({1, 1});
  (void)cache.Lookup({1, 0});
  cache.Insert({1, 9}, make(9), 100);  // Evicts 2 (LRU).
  EXPECT_EQ(cache.Lookup({1, 2}), nullptr);
  EXPECT_NE(cache.Lookup({1, 0}), nullptr);
  cache.Insert({1, 10}, make(10), 100);
  cache.Insert({1, 11}, make(11), 100);
  EXPECT_EQ(cache.Lookup({1, 1}), nullptr);
  EXPECT_EQ(*std::static_pointer_cast<const int>(pinned), 1);  // Still alive while held.
  EXPECT_LE(cache.TotalCharge(), cache.capacity());
}

class TableTest : public ::testing::TestWithParam<IoBackendKind> {
 protected:
  void SetUp() override {
    options_.io_backend = GetParam();
    io_ = test::MakeBackend(GetParam());
    if (!io_) GTEST_SKIP() << "backend unavailable";
    cache_ = std::make_unique<LruCache>(8 << 20, 4);
  }

  // Builds file `number` from model (user key -> (seq, type, value)).
  void Build(uint64_t number, const std::map<std::string, std::string>& kv, SequenceNumber seq = 5) {
    std::unique_ptr<sst::TableBuilder> tb;
    ASSERT_TRUE(sst::TableBuilder::Create(options_, io_.get(), TableFileName(dir_.path(), number), &tb).ok());
    for (const auto& [k, v] : kv) tb->Add(MakeInternalKey(k, seq, ValueType::kValue), v);
    ASSERT_TRUE(tb->Finish().ok());
    EXPECT_EQ(tb->FileSize() % kIoAlignment, 0u);
  }

  Status Open(uint64_t number, std::unique_ptr<sst::TableReader>* r) {
    return sst::TableReader::Open(options_, io_.get(), cache_.get(), &stats_, number,
                                  TableFileName(dir_.path(), number), r);
  }

  Options options_;
  std::shared_ptr<IoBackend> io_;
  std::unique_ptr<LruCache> cache_;
  sst::TableStats stats_;
  TempDir dir_;
};

TEST_P(TableTest, PointLookupsIteratorAndLayout) {
  std::map<std::string, std::string> kv;
  std::mt19937 rng(7);
  for (int i = 0; i < 30000; ++i) kv[Key(i * 3)] = std::string(1 + rng() % 300, static_cast<char>('a' + i % 26));
  kv[Key(1)] = std::string(20000, 'L');  // Oversized block (> 4 KiB).
  Build(7, kv);

  struct stat st;
  ASSERT_EQ(::stat(TableFileName(dir_.path(), 7).c_str(), &st), 0);
  EXPECT_EQ(st.st_size % 4096, 0);

  std::unique_ptr<sst::TableReader> r;
  ASSERT_TRUE(Open(7, &r).ok());
  ReadOptions ro;
  for (const auto& [k, v] : kv) {
    bool found = false;
    ValueType t{};
    std::string got;
    ASSERT_TRUE(r->Get(ro, MakeInternalKey(k, kMaxSequenceNumber, kValueTypeForSeek), &found, &t, &got).ok());
    ASSERT_TRUE(found) << k;
    ASSERT_EQ(got, v);
  }
  // Absent keys (in range and out of range), and a snapshot older than the data.
  int found_absent = 0;
  for (int i = 0; i < 30000; ++i) {
    bool found = false;
    ValueType t{};
    std::string got;
    ASSERT_TRUE(r->Get(ro, MakeInternalKey(Key(i * 3 + 2), kMaxSequenceNumber, kValueTypeForSeek), &found, &t, &got).ok());
    found_absent += found;
  }
  EXPECT_EQ(found_absent, 0);
  EXPECT_GT(stats_.bloom_useful.load(), 29000u);  // Bloom rejects ~99%.
  bool found = true;
  ValueType t{};
  std::string got;
  ASSERT_TRUE(r->Get(ro, MakeInternalKey(Key(0), 4, kValueTypeForSeek), &found, &t, &got).ok());
  EXPECT_FALSE(found);

  auto it = r->NewIterator(ro);
  it->SeekToFirst();
  for (const auto& [k, v] : kv) {
    ASSERT_TRUE(it->Valid());
    ASSERT_EQ(ExtractUserKey(it->key()).ToString(), k);
    ASSERT_EQ(it->value().ToString(), v);
    it->Next();
  }
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
  it->Seek(MakeInternalKey(Key(1000), kMaxSequenceNumber, kValueTypeForSeek));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(ExtractUserKey(it->key()).ToString(), Key(1002));
}

TEST_P(TableTest, BlockCacheServesRepeatedReads) {
  std::map<std::string, std::string> kv;
  for (int i = 0; i < 5000; ++i) kv[Key(i)] = std::string(100, 'x');
  Build(9, kv);
  std::unique_ptr<sst::TableReader> r;
  ASSERT_TRUE(Open(9, &r).ok());
  ReadOptions ro;
  bool found;
  ValueType t;
  std::string v;
  ASSERT_TRUE(r->Get(ro, MakeInternalKey(Key(42), kMaxSequenceNumber, kValueTypeForSeek), &found, &t, &v).ok());
  const uint64_t reads = stats_.blocks_read.load();
  const uint64_t hits = cache_->hits();
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(r->Get(ro, MakeInternalKey(Key(42), kMaxSequenceNumber, kValueTypeForSeek), &found, &t, &v).ok());
  }
  EXPECT_EQ(stats_.blocks_read.load(), reads);
  EXPECT_EQ(cache_->hits(), hits + 100);
}

TEST_P(TableTest, CorruptDataBlockIsDetected) {
  std::map<std::string, std::string> kv;
  for (int i = 0; i < 2000; ++i) kv[Key(i)] = std::string(64, 'q');
  Build(11, kv);
  const std::string path = TableFileName(dir_.path(), 11);
  const int fd = ::open(path.c_str(), O_WRONLY);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::pwrite(fd, "\x01\x02\x03", 3, 100), 3);  // Inside data block 0.
  ::close(fd);

  std::unique_ptr<sst::TableReader> r;
  ASSERT_TRUE(Open(11, &r).ok());  // Footer/index/bloom are intact.
  bool found;
  ValueType t;
  std::string v;
  Status s = r->Get(ReadOptions(), MakeInternalKey(Key(0), kMaxSequenceNumber, kValueTypeForSeek), &found, &t, &v);
  EXPECT_TRUE(s.IsCorruption()) << s.ToString();
}

TEST_P(TableTest, CorruptFooterFailsOpen) {
  std::map<std::string, std::string> kv{{"a", "b"}};
  Build(13, kv);
  const std::string path = TableFileName(dir_.path(), 13);
  struct stat st;
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  const int fd = ::open(path.c_str(), O_WRONLY);
  ASSERT_EQ(::pwrite(fd, "X", 1, st.st_size - 1), 1);  // Magic.
  ::close(fd);
  std::unique_ptr<sst::TableReader> r;
  EXPECT_TRUE(Open(13, &r).IsCorruption());
}

INSTANTIATE_TEST_SUITE_P(Backends, TableTest, ::testing::Values(IoBackendKind::kPread, IoBackendKind::kUring),
                         [](const auto& p) { return test::BackendName(p.param); });

}  // namespace
}  // namespace lsm
