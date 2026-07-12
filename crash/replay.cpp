// Crash-consistency harness.
//
//   crash_replay eio   --dir D   Fail the N-th fsync/fdatasync (crash/eio_shim.c)
//                                for N = 0,1,2,...; the engine must terminate
//                                (fsyncgate) and recover every acknowledged write.
//   crash_replay kill  --dir D   SIGKILL the engine at random points mid-workload.
//   crash_replay trace --dir D   Record the workload's disk-mutating syscalls
//                                (crash/tracer.cpp), materialise every crash
//                                state of the trace, recover each one.
//
// Invariants checked after every recovery:
//   1. Every acknowledged write before the crash point is present and correct
//      (the single in-flight, unacknowledged operation may or may not be).
//   2. No ghost key/value appears that was never written.
//
// The workload runs in a child process (re-exec of this binary) so that it
// can be killed or preloaded without affecting the checker. It reports
//   "B <op> <P|D> <key> <value>"  before issuing an operation and
//   "A <op>"                      after it was acknowledged
// over a pipe (eio/kill) or as trace marks (trace).
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "lsm/db.hpp"

#ifndef LSMKV_EIO_SHIM_PATH
#define LSMKV_EIO_SHIM_PATH "libeio_shim.so"
#endif
#ifndef LSMKV_TRACER_PATH
#define LSMKV_TRACER_PATH "liblsmkv_tracer.so"
#endif

extern char** environ;

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Command line

struct Args {
  std::string mode;
  std::string dir;
  int ops = 300;
  int rounds = 12;
  int max_points = 120;
  uint64_t seed = 1;
  int ack_fd = -1;
  std::string backend = "pread";
  bool verbose = false;
};

[[noreturn]] void Usage() {
  std::fprintf(stderr,
               "usage: crash_replay <eio|kill|trace> --dir DIR [--ops N] [--rounds N] [--max-points N]\n"
               "                    [--seed N] [--backend auto|pread|uring] [--verbose]\n");
  std::exit(64);
}

Args Parse(int argc, char** argv) {
  if (argc < 2) Usage();
  Args a;
  a.mode = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string k = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) Usage();
      return argv[++i];
    };
    if (k == "--dir") a.dir = val();
    else if (k == "--ops") a.ops = std::stoi(val());
    else if (k == "--rounds") a.rounds = std::stoi(val());
    else if (k == "--max-points") a.max_points = std::stoi(val());
    else if (k == "--seed") a.seed = std::stoull(val());
    else if (k == "--ack-fd") a.ack_fd = std::stoi(val());
    else if (k == "--backend") a.backend = val();
    else if (k == "--verbose") a.verbose = true;
    else Usage();
  }
  if (a.dir.empty()) Usage();
  a.dir = fs::absolute(a.dir).lexically_normal().string();
  while (a.dir.size() > 1 && a.dir.back() == '/') a.dir.pop_back();
  return a;
}

lsm::IoBackendKind BackendKind(const std::string& s) {
  if (s == "auto") return lsm::IoBackendKind::kAuto;
  if (s == "uring") return lsm::IoBackendKind::kUring;
  return lsm::IoBackendKind::kPread;
}

// Small buffers so a few hundred operations exercise memtable switches,
// flushes, manifest rewrites and multi-level compactions.
lsm::Options WorkloadOptions(lsm::IoBackendKind backend) {
  lsm::Options o;
  o.io_backend = backend;
  o.write_buffer_size = 64 * 1024;
  o.target_file_size = 64 * 1024;
  o.max_bytes_for_level_base = 128 * 1024;
  o.level0_compaction_trigger = 2;
  o.wal_preallocate_bytes = 128 * 1024;
  o.block_cache_capacity = 1 << 20;
  o.uring_fixed_buffers = 2;
  o.uring_num_rings = 1;
  o.abort_on_fsync_error = true;
  return o;
}

// ---------------------------------------------------------------------------
// Workload (child side)

using MarkFn = void (*)(const char*, size_t);

struct Reporter {
  int fd = -1;
  MarkFn mark = nullptr;
  void Line(const std::string& s) const {
    if (mark != nullptr) {
      mark(s.data(), s.size());
      return;
    }
    std::string l = s + "\n";
    const char* p = l.data();
    size_t n = l.size();
    while (n > 0) {
      const ssize_t w = ::write(fd, p, n);
      if (w < 0) {
        if (errno == EINTR) continue;
        _exit(5);
      }
      p += w;
      n -= static_cast<size_t>(w);
    }
  }
};

std::string MakeValue(int op, size_t len) {
  std::string v = "v" + std::to_string(op) + "-";
  while (v.size() < len) v.push_back(static_cast<char>('a' + (op * 31 + static_cast<int>(v.size())) % 26));
  return v;
}

[[noreturn]] void RunChild(const Args& a) {
  Reporter rep;
  rep.fd = a.ack_fd;
  if (a.ack_fd < 0) {
    void* sym = dlsym(RTLD_DEFAULT, "lsmkv_trace_mark");
    std::memcpy(&rep.mark, &sym, sizeof(sym));
    if (rep.mark == nullptr) {
      std::fprintf(stderr, "child: no ack fd and tracer not preloaded\n");
      _exit(4);
    }
  }
  std::unique_ptr<lsm::DB> db;
  lsm::Status s = lsm::DB::Open(WorkloadOptions(BackendKind(a.backend)), a.dir, &db);
  if (!s.ok()) {
    std::fprintf(stderr, "child: open failed: %s\n", s.ToString().c_str());
    _exit(2);
  }
  std::mt19937_64 rng(a.seed);
  lsm::WriteOptions wo;
  wo.sync = true;
  for (int op = 0; op < a.ops; ++op) {
    char key[16];
    std::snprintf(key, sizeof(key), "key%03d", static_cast<int>(rng() % 150));
    const bool del = rng() % 8 == 0;
    const std::string value = del ? "" : MakeValue(op, 200 + rng() % 1300);
    rep.Line("B " + std::to_string(op) + (del ? " D " : " P ") + key + " " + value);
    s = del ? db->Delete(wo, key) : db->Put(wo, key, value);
    if (!s.ok()) {
      // With abort_on_fsync_error the engine should have terminated already.
      std::fprintf(stderr, "child: op %d failed without terminating: %s\n", op, s.ToString().c_str());
      _exit(3);
    }
    rep.Line("A " + std::to_string(op));
    if (op % 60 == 59 && !(s = db->Flush()).ok()) {
      std::fprintf(stderr, "child: flush failed: %s\n", s.ToString().c_str());
      _exit(3);
    }
  }
  if (!(s = db->CompactAll()).ok()) {
    std::fprintf(stderr, "child: compaction failed: %s\n", s.ToString().c_str());
    _exit(3);
  }
  db.reset();
  _exit(0);
}

// ---------------------------------------------------------------------------
// Model + invariant checks (parent side)

struct Event {
  int op = -1;
  bool del = false;
  std::string key, value;
  bool acked = false;
};

class History {
 public:
  void Feed(const std::string& line) {
    if (line.size() < 3) return;
    if (line[0] == 'B') {
      Event e;
      size_t p = 2;
      const size_t sp1 = line.find(' ', p);
      e.op = std::stoi(line.substr(p, sp1 - p));
      e.del = line[sp1 + 1] == 'D';
      const size_t ks = sp1 + 3;
      const size_t sp2 = line.find(' ', ks);
      e.key = line.substr(ks, sp2 - ks);
      e.value = sp2 == std::string::npos ? "" : line.substr(sp2 + 1);
      events_.push_back(std::move(e));
    } else if (line[0] == 'A') {
      const int op = std::stoi(line.substr(2));
      for (auto it = events_.rbegin(); it != events_.rend(); ++it) {
        if (it->op == op) {
          it->acked = true;
          break;
        }
      }
    }
  }
  [[nodiscard]] size_t acked() const {
    return static_cast<size_t>(std::count_if(events_.begin(), events_.end(), [](const Event& e) { return e.acked; }));
  }
  [[nodiscard]] const std::vector<Event>& events() const { return events_; }

 private:
  std::vector<Event> events_;
};

// Returns an empty string on success, otherwise a description of the
// violated invariant.
std::string Verify(const std::string& dir, const History& h) {
  lsm::Options o = WorkloadOptions(lsm::IoBackendKind::kPread);
  o.abort_on_fsync_error = false;
  o.wal_preallocate_bytes = 0;
  std::unique_ptr<lsm::DB> db;
  lsm::Status s = lsm::DB::Open(o, dir, &db);
  if (!s.ok()) return "recovery failed: " + s.ToString();

  std::map<std::string, std::optional<std::string>> model;  // Acknowledged state.
  std::map<std::string, std::set<std::string>> written;      // Every value ever attempted.
  const Event* inflight = nullptr;
  for (const auto& e : h.events()) {
    written[e.key].insert(e.del ? std::string("\x01<deleted>") : e.value);
    if (e.acked) {
      model[e.key] = e.del ? std::nullopt : std::optional<std::string>(e.value);
    } else {
      if (inflight != nullptr) return "workload has two unacknowledged operations";
      inflight = &e;
    }
  }

  for (const auto& [key, values] : written) {
    std::string got;
    s = db->Get(lsm::ReadOptions(), key, &got);
    if (!s.ok() && !s.IsNotFound()) return "Get(" + key + "): " + s.ToString();
    const std::optional<std::string> actual = s.ok() ? std::optional<std::string>(got) : std::nullopt;
    const auto m = model.find(key);
    const std::optional<std::string> expected = m == model.end() ? std::nullopt : m->second;
    if (actual == expected) continue;
    if (inflight != nullptr && inflight->key == key) {
      const std::optional<std::string> alt =
          inflight->del ? std::nullopt : std::optional<std::string>(inflight->value);
      if (actual == alt) continue;
    }
    auto show = [](const std::optional<std::string>& v) {
      return v ? "\"" + v->substr(0, 24) + (v->size() > 24 ? "...\"" : "\"") : std::string("<absent>");
    };
    return "invariant 1 violated for " + key + ": expected " + show(expected) + ", got " + show(actual);
  }

  auto it = db->NewIterator(lsm::ReadOptions());
  size_t n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++n) {
    const std::string k = it->key().ToString();
    auto w = written.find(k);
    if (w == written.end()) return "invariant 2 violated: ghost key " + k;
    if (!w->second.contains(it->value().ToString())) return "invariant 2 violated: ghost value for " + k;
  }
  if (!it->status().ok()) return "iterator: " + it->status().ToString();
  return {};
}

// ---------------------------------------------------------------------------
// Child process plumbing

struct ChildResult {
  int status = 0;
  History history;
};

pid_t SpawnChild(const Args& a, const std::string& dir, uint64_t seed, int ack_fd,
                 const std::vector<std::pair<std::string, std::string>>& env) {
  const pid_t pid = ::fork();
  if (pid < 0) {
    std::perror("fork");
    std::exit(1);
  }
  if (pid > 0) return pid;
  std::vector<std::string> args = {"/proc/self/exe",
                                   "child",
                                   "--dir",
                                   dir,
                                   "--ops",
                                   std::to_string(a.ops),
                                   "--seed",
                                   std::to_string(seed),
                                   "--ack-fd",
                                   std::to_string(ack_fd),
                                   "--backend",
                                   a.backend};
  std::vector<std::string> envs;
  for (char** e = environ; *e != nullptr; ++e) {
    const std::string kv = *e;
    const std::string name = kv.substr(0, kv.find('='));
    if (std::none_of(env.begin(), env.end(), [&](const auto& p) { return p.first == name; })) envs.push_back(kv);
  }
  for (const auto& [k, v] : env) envs.push_back(k + "=" + v);
  std::vector<char*> argv, envp;
  for (auto& s : args) argv.push_back(s.data());
  argv.push_back(nullptr);
  for (auto& s : envs) envp.push_back(s.data());
  envp.push_back(nullptr);
  if (!a.verbose) {
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) ::dup2(devnull, STDERR_FILENO);
  }
  ::execve("/proc/self/exe", argv.data(), envp.data());
  _exit(127);
}

std::string DescribeStatus(int st) {
  if (WIFEXITED(st)) return "exit(" + std::to_string(WEXITSTATUS(st)) + ")";
  if (WIFSIGNALED(st)) return std::string("signal ") + strsignal(WTERMSIG(st));
  return "status " + std::to_string(st);
}

void FreshDir(const std::string& d) {
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d);
}

std::string ShimPath(const char* env, const char* built_in) {
  const char* e = std::getenv(env);
  return e != nullptr ? e : built_in;
}

// Runs a child with an ack pipe. If kill_after > 0 the child is SIGKILLed
// once that many acknowledgements arrived.
ChildResult RunPiped(const Args& a, const std::string& dir, uint64_t seed,
                     const std::vector<std::pair<std::string, std::string>>& env, size_t kill_after) {
  int p[2];
  if (::pipe(p) != 0) {
    std::perror("pipe");
    std::exit(1);
  }
  const pid_t pid = SpawnChild(a, dir, seed, p[1], env);
  ::close(p[1]);
  ChildResult r;
  std::string buf;
  char chunk[8192];
  bool killed = false;
  for (;;) {
    const ssize_t n = ::read(p[0], chunk, sizeof(chunk));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    buf.append(chunk, static_cast<size_t>(n));
    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
      r.history.Feed(buf.substr(0, nl));
      buf.erase(0, nl + 1);
    }
    if (kill_after > 0 && !killed && r.history.acked() >= kill_after) {
      ::kill(pid, SIGKILL);
      killed = true;  // Keep draining: acks already in the pipe were real.
    }
  }
  ::close(p[0]);
  ::waitpid(pid, &r.status, 0);
  return r;
}

// Every child path needs this so ASan-instrumented builds tolerate the shim
// appearing before the sanitizer runtime in the link order.
std::vector<std::pair<std::string, std::string>> BaseEnv() {
  return {{"ASAN_OPTIONS", "verify_asan_link_order=0:detect_leaks=0"}};
}

// ---------------------------------------------------------------------------
// eio

int RunEio(const Args& a) {
  const std::string shim = ShimPath("LSMKV_EIO_SHIM", LSMKV_EIO_SHIM_PATH);
  int failures = 0, runs = 0;
  for (int n = 0; n < 100000; n += n < 24 ? 1 : 9) {
    if (runs >= a.max_points) break;
    const std::string dir = a.dir + "/eio_" + std::to_string(n);
    FreshDir(dir);
    auto env = BaseEnv();
    env.emplace_back("LD_PRELOAD", shim);
    env.emplace_back("INJECT_EIO_AFTER_COUNT", std::to_string(n));
    env.emplace_back("INJECT_EIO_ERRNO", n % 3 == 2 ? "ENOSPC" : "EIO");
    ChildResult r = RunPiped(a, dir, a.seed + static_cast<uint64_t>(n), env, 0);
    ++runs;
    const bool clean_exit = WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0;
    const bool aborted = WIFSIGNALED(r.status) && WTERMSIG(r.status) == SIGABRT;
    const bool open_failed = WIFEXITED(r.status) && WEXITSTATUS(r.status) == 2;
    std::string verdict;
    if (clean_exit) {
      std::printf("eio  after=%-4d child completed without reaching the injection point; done\n", n);
      std::error_code ec;
      fs::remove_all(dir, ec);
      break;
    }
    if (!aborted && !open_failed) {
      verdict = "engine did not terminate on fsync failure (" + DescribeStatus(r.status) + ")";
    } else if (open_failed && r.history.acked() != 0) {
      verdict = "open failed after acknowledging writes";
    } else {
      verdict = Verify(dir, r.history);
    }
    std::printf("eio  after=%-4d %-8s child=%-16s acked=%-4zu %s\n", n, n % 3 == 2 ? "ENOSPC" : "EIO",
                DescribeStatus(r.status).c_str(), r.history.acked(), verdict.empty() ? "OK" : verdict.c_str());
    if (!verdict.empty()) ++failures;
    else {
      std::error_code ec;
      fs::remove_all(dir, ec);
    }
  }
  std::printf("eio: %d runs, %d failures\n", runs, failures);
  return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// kill

int RunKill(const Args& a) {
  std::mt19937_64 rng(a.seed);
  int failures = 0;
  for (int round = 0; round < a.rounds; ++round) {
    const std::string dir = a.dir + "/kill_" + std::to_string(round);
    FreshDir(dir);
    const size_t kill_after = 1 + rng() % static_cast<uint64_t>(std::max(1, a.ops - 1));
    ChildResult r = RunPiped(a, dir, a.seed + static_cast<uint64_t>(round), BaseEnv(), kill_after);
    const std::string verdict = Verify(dir, r.history);
    std::printf("kill round=%-3d kill_after=%-4zu child=%-16s acked=%-4zu %s\n", round, kill_after,
                DescribeStatus(r.status).c_str(), r.history.acked(), verdict.empty() ? "OK" : verdict.c_str());
    if (!verdict.empty()) ++failures;
    else {
      std::error_code ec;
      fs::remove_all(dir, ec);
    }
  }
  std::printf("kill: %d rounds, %d failures\n", a.rounds, failures);
  return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// trace

enum TraceOp : uint8_t { kCreate = 1, kWrite, kTruncate, kExtend, kSync, kRename, kUnlink, kMark };

struct TraceRec {
  TraceOp op;
  std::string p1, p2;
  uint64_t off = 0, len = 0;
  std::string data;
};

bool ReadTrace(const std::string& path, std::vector<TraceRec>* out) {
  std::string raw;
  {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    char buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) raw.append(buf, n);
    std::fclose(f);
  }
  size_t p = 0;
  auto need = [&](size_t n) { return p + n <= raw.size(); };
  auto u32 = [&] {
    uint32_t v;
    std::memcpy(&v, raw.data() + p, 4);
    p += 4;
    return v;
  };
  auto u64 = [&] {
    uint64_t v;
    std::memcpy(&v, raw.data() + p, 8);
    p += 8;
    return v;
  };
  while (p < raw.size()) {
    TraceRec r;
    if (!need(5)) return false;
    r.op = static_cast<TraceOp>(raw[p++]);
    uint32_t l1 = u32();
    if (!need(l1 + 4)) return false;
    r.p1 = raw.substr(p, l1);
    p += l1;
    uint32_t l2 = u32();
    if (!need(l2 + 16)) return false;
    r.p2 = raw.substr(p, l2);
    p += l2;
    r.off = u64();
    r.len = u64();
    if (r.op == kWrite) {
      if (!need(r.len)) return false;
      r.data = raw.substr(p, r.len);
      p += r.len;
    }
    out->push_back(std::move(r));
  }
  return true;
}

// Applies ops[0..k) to `dir`. With durable_only, an operation is applied only
// if a later sync (still before k) made it durable: data ops need an fsync of
// the same file, namespace ops (create/rename/unlink) an fsync of the
// directory. This is the weakest state a POSIX filesystem must preserve.
void Materialize(const std::vector<TraceRec>& ops, size_t k, bool durable_only, const std::string& dir) {
  std::vector<size_t> next_sync(ops.size(), SIZE_MAX);
  {
    std::map<std::string, size_t> nearest;
    for (size_t i = ops.size(); i-- > 0;) {
      const TraceRec& r = ops[i];
      if (r.op == kSync) {
        nearest[r.p1] = i;
        continue;
      }
      const bool ns = r.op == kCreate || r.op == kRename || r.op == kUnlink;
      auto it = nearest.find(ns ? std::string(".") : r.p1);
      if (it != nearest.end()) next_sync[i] = it->second;
    }
  }
  auto full = [&](const std::string& rel) { return rel == "." ? dir : dir + "/" + rel; };
  for (size_t i = 0; i < k; ++i) {
    const TraceRec& r = ops[i];
    if (r.op == kSync || r.op == kMark) continue;
    if (durable_only && next_sync[i] >= k) continue;
    const std::string path = full(r.p1);
    switch (r.op) {
      case kCreate: {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT, 0644);
        if (fd >= 0) ::close(fd);
        break;
      }
      case kWrite: {
        const int fd = ::open(path.c_str(), O_WRONLY);
        if (fd < 0) break;  // Its creation never became durable.
        (void)!::pwrite(fd, r.data.data(), r.data.size(), static_cast<off_t>(r.off));
        ::close(fd);
        break;
      }
      case kTruncate:
        (void)!::truncate(path.c_str(), static_cast<off_t>(r.off));
        break;
      case kExtend: {
        struct stat st;
        if (::stat(path.c_str(), &st) == 0 && static_cast<uint64_t>(st.st_size) < r.off) {
          (void)!::truncate(path.c_str(), static_cast<off_t>(r.off));
        }
        break;
      }
      case kRename:
        (void)::rename(path.c_str(), full(r.p2).c_str());
        break;
      case kUnlink:
        (void)::unlink(path.c_str());
        break;
      default:
        break;
    }
  }
}

int RunTrace(const Args& a) {
  const std::string rec_dir = a.dir + "/trace_record";
  const std::string trace_file = a.dir + "/trace.bin";
  FreshDir(rec_dir);
  Args child = a;
  child.backend = "pread";  // io_uring submissions are invisible to an LD_PRELOAD tracer.
  if (a.ops == 300) child.ops = 150;
  auto env = BaseEnv();
  env.emplace_back("LD_PRELOAD", ShimPath("LSMKV_TRACER", LSMKV_TRACER_PATH));
  env.emplace_back("LSMKV_TRACE_FILE", trace_file);
  env.emplace_back("LSMKV_TRACE_ROOT", rec_dir);
  const pid_t pid = SpawnChild(child, rec_dir, a.seed, -1, env);
  int st = 0;
  ::waitpid(pid, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    std::printf("trace: recording run failed: %s\n", DescribeStatus(st).c_str());
    return 1;
  }

  std::vector<TraceRec> all;
  if (!ReadTrace(trace_file, &all)) {
    std::printf("trace: cannot parse %s\n", trace_file.c_str());
    return 1;
  }
  // Split marks out; remember how many real ops preceded each.
  std::vector<TraceRec> ops;
  std::vector<std::pair<size_t, std::string>> marks;
  size_t syncs = 0, bytes = 0;
  for (auto& r : all) {
    if (r.op == kMark) {
      marks.emplace_back(ops.size(), r.p1);
    } else {
      syncs += r.op == kSync;
      bytes += r.data.size();
      ops.push_back(std::move(r));
    }
  }
  std::printf("trace: %zu ops (%zu syncs, %zu KiB written), %zu marks\n", ops.size(), syncs, bytes / 1024,
              marks.size());

  // Crash points: every point right after a sync (where durability changes)
  // plus an even spread, capped at max_points.
  std::set<size_t> points{0, ops.size()};
  std::vector<size_t> after_sync;
  for (size_t i = 0; i < ops.size(); ++i) {
    if (ops[i].op == kSync) after_sync.push_back(i + 1);
  }
  const size_t budget = static_cast<size_t>(std::max(2, a.max_points));
  const size_t stride_s = std::max<size_t>(1, after_sync.size() / (budget / 2 + 1));
  for (size_t i = 0; i < after_sync.size(); i += stride_s) points.insert(after_sync[i]);
  const size_t stride = std::max<size_t>(1, ops.size() / (budget / 2 + 1));
  for (size_t i = 0; i < ops.size(); i += stride) points.insert(i);

  int failures = 0, checks = 0;
  const std::string scratch = a.dir + "/trace_check";
  for (size_t k : points) {
    History h;
    for (const auto& [pos, line] : marks) {
      if (pos <= k) h.Feed(line);
    }
    for (int durable = 0; durable < 2; ++durable) {
      FreshDir(scratch);
      Materialize(ops, k, durable == 1, scratch);
      const std::string verdict = Verify(scratch, h);
      ++checks;
      if (!verdict.empty() || a.verbose) {
        std::printf("trace crash@%-5zu model=%-7s acked=%-4zu %s\n", k, durable ? "durable" : "prefix", h.acked(),
                    verdict.empty() ? "OK" : verdict.c_str());
      }
      if (!verdict.empty()) {
        ++failures;
        const std::string keep = a.dir + "/trace_fail_" + std::to_string(k) + (durable ? "_durable" : "_prefix");
        FreshDir(keep);
        Materialize(ops, k, durable == 1, keep);  // Pristine copy for debugging.
      }
    }
  }
  std::error_code ec;
  fs::remove_all(scratch, ec);
  std::printf("trace: %zu crash points, %d recoveries checked, %d failures\n", points.size(), checks, failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const Args a = Parse(argc, argv);
  if (a.mode == "child") RunChild(a);
  fs::create_directories(a.dir);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  if (a.mode == "eio") return RunEio(a);
  if (a.mode == "kill") return RunKill(a);
  if (a.mode == "trace") return RunTrace(a);
  Usage();
}
