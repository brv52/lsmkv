#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "memtable/memtable.hpp"
#include "memtable/skiplist.hpp"

namespace lsm {
namespace {

struct U64Cmp {
  int operator()(uint64_t a, uint64_t b) const noexcept { return a < b ? -1 : (a > b ? 1 : 0); }
};
using U64List = SkipList<uint64_t, U64Cmp>;

TEST(SkipList, Empty) {
  ConcurrentArena arena;
  U64List list(U64Cmp{}, &arena);
  EXPECT_FALSE(list.Contains(10));
  U64List::Iterator it(&list);
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
  it.Seek(100);
  EXPECT_FALSE(it.Valid());
}

TEST(SkipList, InsertAndLookup) {
  ConcurrentArena arena;
  U64List list(U64Cmp{}, &arena);
  std::mt19937_64 rng(301);
  std::set<uint64_t> model;
  for (int i = 0; i < 20000; ++i) {
    const uint64_t k = rng() % 50000;
    if (model.insert(k).second) list.Insert(k);
  }
  for (uint64_t k = 0; k < 50000; ++k) ASSERT_EQ(list.Contains(k), model.contains(k)) << k;

  U64List::Iterator it(&list);
  it.SeekToFirst();
  for (uint64_t k : model) {
    ASSERT_TRUE(it.Valid());
    ASSERT_EQ(it.key(), k);
    it.Next();
  }
  EXPECT_FALSE(it.Valid());

  for (uint64_t probe : {0ull, 1ull, 777ull, 49999ull, 60000ull}) {
    it.Seek(probe);
    auto m = model.lower_bound(probe);
    if (m == model.end()) {
      EXPECT_FALSE(it.Valid());
    } else {
      ASSERT_TRUE(it.Valid());
      EXPECT_EQ(it.key(), *m);
    }
  }
}

TEST(SkipList, ConcurrentInsertsAreAllVisibleAndSorted) {
  ConcurrentArena arena;
  U64List list(U64Cmp{}, &arena);
  constexpr int kThreads = 8;
  constexpr uint64_t kPerThread = 25000;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      // Interleaved key spaces maximise CAS contention on shared splices.
      for (uint64_t i = 0; i < kPerThread; ++i) list.Insert(i * kThreads + static_cast<uint64_t>(t));
    });
  }
  for (auto& th : threads) th.join();

  U64List::Iterator it(&list);
  it.SeekToFirst();
  uint64_t expect = 0;
  for (; it.Valid(); it.Next(), ++expect) ASSERT_EQ(it.key(), expect);
  EXPECT_EQ(expect, kThreads * kPerThread);
}

TEST(SkipList, ReadersObserveSortedPrefixWhileWritersInsert) {
  ConcurrentArena arena;
  U64List list(U64Cmp{}, &arena);
  std::atomic<bool> done{false};
  std::atomic<uint64_t> violations{0};

  std::thread reader([&] {
    while (!done.load(std::memory_order_acquire)) {
      U64List::Iterator it(&list);
      it.SeekToFirst();
      uint64_t prev = 0;
      bool first = true;
      for (; it.Valid(); it.Next()) {
        if (!first && it.key() <= prev) violations.fetch_add(1);
        prev = it.key();
        first = false;
      }
    }
  });
  std::vector<std::thread> writers;
  for (int t = 0; t < 4; ++t) {
    writers.emplace_back([&, t] {
      std::mt19937_64 rng(static_cast<uint64_t>(t) + 1);
      for (int i = 0; i < 20000; ++i) list.Insert((rng() << 2) | static_cast<uint64_t>(t));  // Unique per thread.
    });
  }
  for (auto& w : writers) w.join();
  done.store(true, std::memory_order_release);
  reader.join();
  EXPECT_EQ(violations.load(), 0u);
}

TEST(MemTable, NewestVersionAndTombstones) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "a", "a1");
  mem.Add(2, ValueType::kValue, "b", "b2");
  mem.Add(3, ValueType::kValue, "a", "a3");
  mem.Add(4, ValueType::kDeletion, "b", "");

  std::string v;
  Status s;
  ASSERT_TRUE(mem.Get(LookupKey("a", 10), &v, &s));
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(v, "a3");
  ASSERT_TRUE(mem.Get(LookupKey("a", 2), &v, &s));  // Snapshot before the overwrite.
  EXPECT_EQ(v, "a1");
  EXPECT_FALSE(mem.Get(LookupKey("a", 0), &v, &s));
  ASSERT_TRUE(mem.Get(LookupKey("b", 10), &v, &s));
  EXPECT_TRUE(s.IsNotFound());
  ASSERT_TRUE(mem.Get(LookupKey("b", 3), &v, &s));
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(v, "b2");
  EXPECT_FALSE(mem.Get(LookupKey("c", 10), &v, &s));
}

TEST(MemTable, IteratorYieldsInternalKeyOrder) {
  MemTable mem;
  mem.Add(5, ValueType::kValue, "k2", "x");
  mem.Add(7, ValueType::kValue, "k1", "y");
  mem.Add(9, ValueType::kValue, "k2", "z");
  auto it = mem.NewIterator();
  it->SeekToFirst();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(ExtractUserKey(it->key()).ToString(), "k1");
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(ExtractUserKey(it->key()).ToString(), "k2");
  EXPECT_EQ(ExtractSequence(it->key()), 9u);  // Newest first.
  EXPECT_EQ(it->value().ToString(), "z");
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(ExtractSequence(it->key()), 5u);
  it->Next();
  EXPECT_FALSE(it->Valid());

  it->Seek(MakeInternalKey("k2", 6, kValueTypeForSeek));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(ExtractSequence(it->key()), 5u);
}

TEST(MemTable, ConcurrentAddFromManyThreads) {
  MemTable mem;
  constexpr int kThreads = 8, kPer = 5000;
  std::vector<std::thread> th;
  for (int t = 0; t < kThreads; ++t) {
    th.emplace_back([&, t] {
      for (int i = 0; i < kPer; ++i) {
        const uint64_t seq = static_cast<uint64_t>(t) * kPer + static_cast<uint64_t>(i) + 1;
        const std::string key = "key" + std::to_string(t) + "_" + std::to_string(i);
        mem.Add(seq, ValueType::kValue, key, std::to_string(seq));
      }
    });
  }
  for (auto& x : th) x.join();
  EXPECT_EQ(mem.num_entries(), static_cast<uint64_t>(kThreads * kPer));
  std::string v;
  Status s;
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kPer; i += 97) {
      ASSERT_TRUE(mem.Get(LookupKey("key" + std::to_string(t) + "_" + std::to_string(i), kMaxSequenceNumber), &v, &s));
      EXPECT_EQ(v, std::to_string(static_cast<uint64_t>(t) * kPer + static_cast<uint64_t>(i) + 1));
    }
  }
}

}  // namespace
}  // namespace lsm
