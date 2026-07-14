// db_bench-compatible benchmark driver for lsmkv.
//
// Flag names, key encoding and value generation follow RocksDB's
// tools/db_bench_tool.cc so the same command line produces the same
// workload against both engines (see bench/rocksdb_dbbench.sh):
//
// * Keys: GenerateKeyFromInt — the key index as big-endian bytes (up to 8),
//   right-padded with '0' to --key_size.
// * Values: RandomGenerator — a 1 MiB buffer of CompressibleString pieces
//   built from LevelDB's Random(301), so --compression_ratio means the same.
// * Random keys: per-thread Random64 (std::mt19937_64) seeded with
//   --seed + a global thread counter that keeps increasing across
//   benchmarks, then `Next() % num`.
//
// Output lines mirror db_bench ("fillrandom : X micros/op Y ops/sec ...") and
// a write-amplification report is printed from /proc/self/io at exit.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "lsm/db.hpp"

namespace {

// ---------------------------------------------------------------------------
// Flags

struct Flags {
  std::string benchmarks = "fillrandom,readrandom,overwrite";
  int64_t num = 1000000;
  int64_t reads = -1;
  int64_t writes = -1;
  int threads = 1;
  int key_size = 16;
  int value_size = 100;
  double compression_ratio = 0.5;
  uint64_t seed = 0;
  std::string db = "/tmp/kvbench";
  bool use_existing_db = false;
  bool sync = false;
  bool disable_wal = false;
  int64_t write_buffer_size = 64ll << 20;
  int64_t cache_size = 8ll << 20;
  int bloom_bits = 10;
  int64_t target_file_size_base = 64ll << 20;
  int64_t max_bytes_for_level_base = 256ll << 20;
  int level0_file_num_compaction_trigger = 4;
  bool use_direct_io = true;
  std::string io_backend = "auto";
  bool histogram = true;
  int64_t stats_interval_seconds = 0;
};

bool ParseFlag(const char* arg, Flags* f) {
  if (std::strncmp(arg, "--", 2) != 0) return false;
  const char* eq = std::strchr(arg, '=');
  if (eq == nullptr) return false;
  const std::string name(arg + 2, eq);
  const std::string v(eq + 1);
  auto b = [&] { return v == "1" || v == "true"; };
  if (name == "benchmarks") f->benchmarks = v;
  else if (name == "num") f->num = std::stoll(v);
  else if (name == "reads") f->reads = std::stoll(v);
  else if (name == "writes") f->writes = std::stoll(v);
  else if (name == "threads") f->threads = std::stoi(v);
  else if (name == "key_size") f->key_size = std::stoi(v);
  else if (name == "value_size") f->value_size = std::stoi(v);
  else if (name == "compression_ratio") f->compression_ratio = std::stod(v);
  else if (name == "seed") f->seed = std::stoull(v);
  else if (name == "db") f->db = v;
  else if (name == "use_existing_db") f->use_existing_db = b();
  else if (name == "sync") f->sync = b();
  else if (name == "disable_wal") f->disable_wal = b();
  else if (name == "write_buffer_size") f->write_buffer_size = std::stoll(v);
  else if (name == "cache_size") f->cache_size = std::stoll(v);
  else if (name == "bloom_bits") f->bloom_bits = std::stoi(v);
  else if (name == "target_file_size_base") f->target_file_size_base = std::stoll(v);
  else if (name == "max_bytes_for_level_base") f->max_bytes_for_level_base = std::stoll(v);
  else if (name == "level0_file_num_compaction_trigger") f->level0_file_num_compaction_trigger = std::stoi(v);
  else if (name == "use_direct_io_for_flush_and_compaction" || name == "use_direct_reads" || name == "use_direct_io")
    f->use_direct_io = b();
  else if (name == "io_backend") f->io_backend = v;
  else if (name == "histogram") f->histogram = b();
  else if (name == "stats_interval_seconds") f->stats_interval_seconds = std::stoll(v);
  // Accepted for command-line compatibility with db_bench; no effect here.
  else if (name == "compression_type" || name == "statistics" || name == "max_background_jobs" ||
           name == "max_write_buffer_number" || name == "open_files" || name == "report_file") {
  } else {
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// RocksDB-compatible generators

// LevelDB/RocksDB util/random.h: Lehmer (Park–Miller) generator.
class Random {
 public:
  explicit Random(uint32_t s) : seed_(s & 0x7fffffffu) {
    if (seed_ == 0 || seed_ == 2147483647u) seed_ = 1;
  }
  uint32_t Next() {
    constexpr uint32_t M = 2147483647u;
    constexpr uint64_t A = 16807;
    const uint64_t product = seed_ * A;
    seed_ = static_cast<uint32_t>((product >> 31) + (product & M));
    if (seed_ > M) seed_ -= M;
    return seed_;
  }
  uint32_t Uniform(int n) { return Next() % static_cast<uint32_t>(n); }

 private:
  uint32_t seed_;
};

// test::RandomString / test::CompressibleString.
std::string CompressibleString(Random* rnd, double ratio, int len) {
  int raw = static_cast<int>(len * ratio);
  if (raw < 1) raw = 1;
  std::string raw_data;
  raw_data.reserve(static_cast<size_t>(raw));
  for (int i = 0; i < raw; ++i) raw_data.push_back(static_cast<char>(' ' + rnd->Uniform(95)));
  std::string dst;
  while (dst.size() < static_cast<size_t>(len)) dst.append(raw_data);
  dst.resize(static_cast<size_t>(len));
  return dst;
}

// db_bench RandomGenerator.
class RandomGenerator {
 public:
  RandomGenerator(int value_size, double ratio) {
    Random rnd(301);
    while (data_.size() < static_cast<size_t>(std::max(1048576, value_size))) {
      data_.append(CompressibleString(&rnd, ratio, 100));
    }
  }
  lsm::Slice Generate(size_t len) {
    if (pos_ + len > data_.size()) pos_ = 0;
    pos_ += len;
    return {data_.data() + pos_ - len, len};
  }

 private:
  std::string data_;
  size_t pos_ = 0;
};

// db_bench GenerateKeyFromInt (keys_per_prefix == 0).
void GenerateKeyFromInt(uint64_t v, int key_size, std::string* key) {
  key->assign(static_cast<size_t>(key_size), '0');
  const int bytes = std::min(key_size, 8);
  for (int i = 0; i < bytes; ++i) (*key)[static_cast<size_t>(i)] = static_cast<char>((v >> ((bytes - i - 1) * 8)) & 0xff);
}

// ---------------------------------------------------------------------------
// Latency histogram (log-linear buckets, 1us .. ~100s)

class Histogram {
 public:
  void Add(double micros) {
    const int b = Bucket(micros);
    ++buckets_[static_cast<size_t>(b)];
    ++count_;
    sum_ += micros;
    max_ = std::max(max_, micros);
  }
  void Merge(const Histogram& o) {
    for (size_t i = 0; i < kBuckets; ++i) buckets_[i] += o.buckets_[i];
    count_ += o.count_;
    sum_ += o.sum_;
    max_ = std::max(max_, o.max_);
  }
  [[nodiscard]] double Percentile(double p) const {
    const double target = p / 100.0 * static_cast<double>(count_);
    double cum = 0;
    for (size_t i = 0; i < kBuckets; ++i) {
      cum += static_cast<double>(buckets_[i]);
      if (cum >= target) return Upper(static_cast<int>(i));
    }
    return max_;
  }
  [[nodiscard]] std::string ToString() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "Microseconds per op: avg %.2f  P50 %.2f  P99 %.2f  P99.9 %.2f  max %.2f",
                  count_ ? sum_ / static_cast<double>(count_) : 0.0, Percentile(50), Percentile(99),
                  Percentile(99.9), max_);
    return buf;
  }

 private:
  static constexpr size_t kBuckets = 400;
  static int Bucket(double us) {
    if (us < 1) return 0;
    const int b = static_cast<int>(std::log10(us) * 50);  // 50 buckets per decade.
    return std::min(b, static_cast<int>(kBuckets) - 1);
  }
  static double Upper(int b) { return std::pow(10.0, (b + 1) / 50.0); }
  std::vector<uint64_t> buckets_ = std::vector<uint64_t>(kBuckets, 0);
  uint64_t count_ = 0;
  double sum_ = 0, max_ = 0;
};

// ---------------------------------------------------------------------------
// /proc/self/io

struct ProcIo {
  uint64_t rchar = 0, wchar = 0, read_bytes = 0, write_bytes = 0;
  static ProcIo Read() {
    ProcIo r;
    std::ifstream f("/proc/self/io");
    std::string k;
    uint64_t v;
    while (f >> k >> v) {
      if (k == "rchar:") r.rchar = v;
      else if (k == "wchar:") r.wchar = v;
      else if (k == "read_bytes:") r.read_bytes = v;
      else if (k == "write_bytes:") r.write_bytes = v;
    }
    return r;
  }
};

// ---------------------------------------------------------------------------
// Benchmarks

std::atomic<int> g_total_threads{0};

struct ThreadResult {
  int64_t ops = 0;
  int64_t found = 0;
  int64_t bytes = 0;
  Histogram hist;
};

class Benchmark {
 public:
  explicit Benchmark(Flags f) : f_(std::move(f)) {
    if (f_.seed == 0) f_.seed = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
  }

  int Run() {
    std::printf("kvbench (lsmkv) — keys: %d bytes, values: %d bytes (%.0f%% compressible), entries: %" PRId64
                ", seed: %" PRIu64 "\n",
                f_.key_size, f_.value_size, (1 - f_.compression_ratio) * 100, f_.num, f_.seed);
    if (!f_.use_existing_db) (void)lsm::DB::Destroy(f_.db);
    if (Status s = Open(); !s.ok()) {
      std::fprintf(stderr, "open %s: %s\n", f_.db.c_str(), s.ToString().c_str());
      return 1;
    }
    std::printf("io backend: %s\n", db_->GetStats().io_backend.c_str());
    const ProcIo io0 = ProcIo::Read();
    std::stringstream ss(f_.benchmarks);
    std::string name;
    while (std::getline(ss, name, ',')) {
      if (name.empty()) continue;
      if (!RunOne(name)) {
        std::fprintf(stderr, "unknown benchmark '%s'\n", name.c_str());
        return 1;
      }
    }
    (void)db_->WaitForCompactions();
    ReportWriteAmp(io0, ProcIo::Read());
    db_.reset();
    return 0;
  }

 private:
  using Status = lsm::Status;

  Status Open() {
    lsm::Options o;
    o.create_if_missing = true;
    o.write_buffer_size = static_cast<size_t>(f_.write_buffer_size);
    o.block_cache_capacity = static_cast<size_t>(f_.cache_size);
    o.bloom_bits_per_key = f_.bloom_bits;
    o.target_file_size = static_cast<size_t>(f_.target_file_size_base);
    o.max_bytes_for_level_base = static_cast<uint64_t>(f_.max_bytes_for_level_base);
    o.level0_compaction_trigger = f_.level0_file_num_compaction_trigger;
    o.use_direct_io = f_.use_direct_io;
    o.io_backend = f_.io_backend == "uring"   ? lsm::IoBackendKind::kUring
                   : f_.io_backend == "pread" ? lsm::IoBackendKind::kPread
                                              : lsm::IoBackendKind::kAuto;
    return lsm::DB::Open(o, f_.db, &db_);
  }

  bool RunOne(const std::string& name) {
    enum class Kind { kWriteRandom, kWriteSeq, kReadRandom, kReadSeq } kind;
    int64_t n;
    if (name == "fillrandom" || name == "overwrite") {
      kind = Kind::kWriteRandom;
      n = f_.writes < 0 ? f_.num : f_.writes;
    } else if (name == "fillseq") {
      kind = Kind::kWriteSeq;
      n = f_.writes < 0 ? f_.num : f_.writes;
    } else if (name == "readrandom") {
      kind = Kind::kReadRandom;
      n = f_.reads < 0 ? f_.num : f_.reads;
    } else if (name == "readseq") {
      kind = Kind::kReadSeq;
      n = f_.reads < 0 ? f_.num : f_.reads;
    } else if (name == "compact") {
      const auto t0 = std::chrono::steady_clock::now();
      Status s = db_->CompactAll();
      const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      std::printf("%-12s : %11.3f seconds%s\n", "compact", secs, s.ok() ? "" : (" " + s.ToString()).c_str());
      return true;
    } else if (name == "waitforcompaction") {
      (void)db_->WaitForCompactions();
      return true;
    } else {
      return false;
    }

    const int threads = std::max(1, f_.threads);
    const int64_t per_thread = n / threads;
    std::vector<ThreadResult> results(static_cast<size_t>(threads));
    std::vector<std::thread> th;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    for (int t = 0; t < threads; ++t) {
      const int seed_index = g_total_threads.fetch_add(1);
      th.emplace_back([&, t, seed_index] {
        ThreadResult& r = results[static_cast<size_t>(t)];
        std::mt19937_64 rand(f_.seed + static_cast<uint64_t>(seed_index));  // db_bench Random64.
        RandomGenerator gen(f_.value_size, f_.compression_ratio);
        std::string key;
        lsm::WriteOptions wo;
        wo.sync = f_.sync;
        wo.disable_wal = f_.disable_wal;
        lsm::ReadOptions ro;
        std::string value;
        std::unique_ptr<lsm::Iterator> it;
        if (kind == Kind::kReadSeq) {
          it = db_->NewIterator(ro);
          it->SeekToFirst();
        }
        ready.fetch_add(1);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int64_t i = 0; i < per_thread; ++i) {
          const auto t0 = std::chrono::steady_clock::now();
          switch (kind) {
            case Kind::kWriteRandom:
            case Kind::kWriteSeq: {
              const uint64_t k = kind == Kind::kWriteSeq ? static_cast<uint64_t>(t * per_thread + i)
                                                         : rand() % static_cast<uint64_t>(f_.num);
              GenerateKeyFromInt(k, f_.key_size, &key);
              const lsm::Slice v = gen.Generate(static_cast<size_t>(f_.value_size));
              Status s = db_->Put(wo, key, v);
              if (!s.ok()) {
                std::fprintf(stderr, "put error: %s\n", s.ToString().c_str());
                std::exit(1);
              }
              r.bytes += static_cast<int64_t>(key.size() + v.size());
              break;
            }
            case Kind::kReadRandom: {
              GenerateKeyFromInt(rand() % static_cast<uint64_t>(f_.num), f_.key_size, &key);
              Status s = db_->Get(ro, key, &value);
              if (s.ok()) {
                ++r.found;
                r.bytes += static_cast<int64_t>(key.size() + value.size());
              } else if (!s.IsNotFound()) {
                std::fprintf(stderr, "get error: %s\n", s.ToString().c_str());
                std::exit(1);
              }
              break;
            }
            case Kind::kReadSeq: {
              if (!it->Valid()) {
                i = per_thread;
                continue;
              }
              r.bytes += static_cast<int64_t>(it->key().size() + it->value().size());
              ++r.found;
              it->Next();
              break;
            }
          }
          ++r.ops;
          if (f_.histogram) {
            r.hist.Add(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
          }
        }
      });
    }
    while (ready.load() < threads) std::this_thread::yield();
    const auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& x : th) x.join();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    ThreadResult total;
    for (const auto& r : results) {
      total.ops += r.ops;
      total.found += r.found;
      total.bytes += r.bytes;
      total.hist.Merge(r.hist);
    }
    const double ops = static_cast<double>(std::max<int64_t>(1, total.ops));
    char extra[128] = "";
    if (kind == Kind::kReadRandom) {
      std::snprintf(extra, sizeof(extra), " (%" PRId64 " of %" PRId64 " found)", total.found, total.ops);
    }
    std::printf("%-12s : %11.3f micros/op %ld ops/sec %.1f seconds %" PRId64 " operations; %6.1f MB/s%s\n",
                name.c_str(), secs * 1e6 * threads / ops, static_cast<long>(ops / secs), secs, total.ops,
                static_cast<double>(total.bytes) / 1048576.0 / secs, extra);
    if (f_.histogram) std::printf("  %s\n", total.hist.ToString().c_str());
    return true;
  }

  void ReportWriteAmp(const ProcIo& a, const ProcIo& b) {
    const lsm::DBStats st = db_->GetStats();
    const double user = static_cast<double>(std::max<uint64_t>(1, st.user_bytes_written));
    const uint64_t dev = b.write_bytes - a.write_bytes;
    std::printf("\n** Write amplification **\n");
    std::printf("  user payload bytes          : %" PRIu64 "\n", st.user_bytes_written);
    std::printf("  WAL bytes (incl. padding)   : %" PRIu64 "  (%.2fx)\n", st.wal_bytes_written,
                static_cast<double>(st.wal_bytes_written) / user);
    std::printf("  flush bytes                 : %" PRIu64 "  (%.2fx)\n", st.flush_bytes_written,
                static_cast<double>(st.flush_bytes_written) / user);
    std::printf("  compaction bytes w / r      : %" PRIu64 " / %" PRIu64 "\n", st.compaction_bytes_written,
                st.compaction_bytes_read);
    std::printf("  engine-submitted bytes      : %" PRIu64 "  (WA %.2fx)\n", st.io_bytes_written,
                static_cast<double>(st.io_bytes_written) / user);
    std::printf("  /proc/self/io write_bytes   : %" PRIu64 "  (device WA %.2fx)\n", dev,
                static_cast<double>(dev) / user);
    std::printf("  group commits               : %" PRIu64 " for %" PRIu64 " writers\n", st.group_commits,
                st.group_commit_writers);
    std::printf("  block cache hit/miss        : %" PRIu64 " / %" PRIu64 ", bloom-filtered lookups %" PRIu64 "\n",
                st.block_cache_hits, st.block_cache_misses, st.bloom_useful);
    std::printf("  files per level             :");
    for (int n : st.files_per_level) std::printf(" %d", n);
    std::printf("\n");
  }

  Flags f_;
  std::unique_ptr<lsm::DB> db_;
};

}  // namespace

int main(int argc, char** argv) {
  Flags f;
  for (int i = 1; i < argc; ++i) {
    if (!ParseFlag(argv[i], &f)) {
      std::fprintf(stderr, "invalid flag: %s\n", argv[i]);
      return 2;
    }
  }
  return Benchmark(std::move(f)).Run();
}
