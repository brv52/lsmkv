// LD_PRELOAD execution tracer for disk-mutating syscalls.
//
// Records, in execution order, every create / write / truncate / extend /
// fsync / rename / unlink that touches files below $LSMKV_TRACE_ROOT into the
// binary log $LSMKV_TRACE_FILE. crash/replay.cpp turns the log into every
// crash state a POSIX filesystem may legally expose and runs recovery on
// each.
//
// Each traced call is executed and logged under one global mutex, so log
// order equals the order in which the kernel saw the calls.
//
// io_uring submissions bypass libc and cannot be observed here; record runs
// therefore force Options::io_backend = kPread.
//
// Record layout (little endian):
//   u8 op | u32 len1 | path1 | u32 len2 | path2 | u64 offset | u64 len | data[len if op == kWrite]
// Paths are relative to the trace root ("." is the root directory itself).
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

enum Op : uint8_t {
  kCreate = 1,
  kWrite = 2,
  kTruncate = 3,
  kExtend = 4,  // fallocate without KEEP_SIZE: size grows, contents zero.
  kSync = 5,    // fsync or fdatasync (both make the file's data durable).
  kRename = 6,
  kUnlink = 7,
  kMark = 8,  // Application annotation (lsmkv_trace_mark).
};

template <class Fn>
Fn Real(const char* name) {
  void* sym = dlsym(RTLD_NEXT, name);
  Fn fn;
  std::memcpy(&fn, &sym, sizeof(fn));
  return fn;
}

struct Tracer {
  std::mutex mu;
  int out = -1;
  std::string root;  // Absolute, no trailing slash.
  std::unordered_map<int, std::string> fds;
};

// Leaked on purpose: interposed calls can run during static destruction.
Tracer& T() {
  static Tracer* t = new Tracer();
  return *t;
}

bool Enabled() { return T().out >= 0; }

void RawWriteAll(int fd, const void* p, size_t n) {
  const auto* c = static_cast<const char*>(p);
  while (n > 0) {
    const long w = syscall(SYS_write, fd, c, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return;
    }
    c += w;
    n -= static_cast<size_t>(w);
  }
}

// REQUIRES: T().mu held.
void Emit(Op op, const std::string& p1, const std::string& p2, uint64_t off, const void* data, uint64_t len) {
  std::string rec;
  rec.reserve(1 + 8 + p1.size() + p2.size() + 16);
  rec.push_back(static_cast<char>(op));
  auto put32 = [&](uint32_t v) { rec.append(reinterpret_cast<const char*>(&v), 4); };
  auto put64 = [&](uint64_t v) { rec.append(reinterpret_cast<const char*>(&v), 8); };
  put32(static_cast<uint32_t>(p1.size()));
  rec += p1;
  put32(static_cast<uint32_t>(p2.size()));
  rec += p2;
  put64(off);
  put64(len);
  RawWriteAll(T().out, rec.data(), rec.size());
  if (op == kWrite && len > 0) RawWriteAll(T().out, data, len);
}

// Maps an absolute or cwd-relative path to a root-relative one.
bool Relative(const char* path, std::string* rel) {
  if (path == nullptr || !Enabled()) return false;
  std::string abs;
  if (path[0] == '/') {
    abs = path;
  } else {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == nullptr) return false;
    abs = std::string(cwd) + "/" + path;
  }
  while (abs.size() > 1 && abs.back() == '/') abs.pop_back();
  const std::string& root = T().root;
  if (abs == root) {
    *rel = ".";
    return true;
  }
  if (abs.size() > root.size() && abs.compare(0, root.size(), root) == 0 && abs[root.size()] == '/') {
    *rel = abs.substr(root.size() + 1);
    return true;
  }
  return false;
}

bool TracedFd(int fd, std::string* rel) {
  auto it = T().fds.find(fd);
  if (it == T().fds.end()) return false;
  *rel = it->second;
  return true;
}

bool Exists(const char* path) {
  struct stat st;
  return syscall(SYS_newfstatat, AT_FDCWD, path, &st, 0) == 0;
}

__attribute__((constructor)) void TracerInit() {
  const char* file = getenv("LSMKV_TRACE_FILE");
  const char* root = getenv("LSMKV_TRACE_ROOT");
  if (file == nullptr || root == nullptr) return;
  char resolved[PATH_MAX];
  if (realpath(root, resolved) == nullptr) return;
  Tracer& t = T();
  t.root = resolved;
  t.out = static_cast<int>(syscall(SYS_openat, AT_FDCWD, file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
}

int DoOpen(int (*real)(int, const char*, int, ...), int dirfd, const char* path, int flags, mode_t mode) {
  std::string rel;
  const bool traced = (dirfd == AT_FDCWD || (path && path[0] == '/')) && Relative(path, &rel);
  if (!traced) return real(dirfd, path, flags, mode);
  std::lock_guard lk(T().mu);
  const bool existed = Exists(path);
  const int fd = real(dirfd, path, flags, mode);
  if (fd >= 0) {
    T().fds[fd] = rel;
    if (!existed && (flags & O_CREAT)) Emit(kCreate, rel, {}, 0, nullptr, 0);
    if (existed && (flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY) Emit(kTruncate, rel, {}, 0, nullptr, 0);
  }
  return fd;
}

mode_t ModeArg(int flags, va_list ap) {
  if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) return static_cast<mode_t>(va_arg(ap, int));
  return 0;
}

template <class F>
ssize_t TracedWrite(int fd, const void* buf, int64_t off, F&& do_write) {
  std::string rel;
  std::lock_guard lk(T().mu);
  if (!TracedFd(fd, &rel)) return do_write();
  const int64_t pos = off >= 0 ? off : static_cast<int64_t>(syscall(SYS_lseek, fd, 0, SEEK_CUR));
  const ssize_t r = do_write();
  if (r > 0) Emit(kWrite, rel, {}, static_cast<uint64_t>(pos), buf, static_cast<uint64_t>(r));
  return r;
}

std::string Gather(const struct iovec* iov, int cnt, size_t bytes) {
  std::string out;
  out.reserve(bytes);
  for (int i = 0; i < cnt && out.size() < bytes; ++i) {
    const size_t take = std::min(bytes - out.size(), iov[i].iov_len);
    out.append(static_cast<const char*>(iov[i].iov_base), take);
  }
  return out;
}

template <class F>
int TracedPathOp(Op op, const char* a, const char* b, F&& do_op) {
  std::string ra, rb;
  const bool ta = Relative(a, &ra);
  const bool tb = b != nullptr && Relative(b, &rb);
  if (!ta && !tb) return do_op();
  std::lock_guard lk(T().mu);
  const int r = do_op();
  if (r == 0) Emit(op, ra, rb, 0, nullptr, 0);
  return r;
}

}  // namespace

extern "C" {

void lsmkv_trace_mark(const char* s, size_t n) {
  if (!Enabled()) return;
  std::lock_guard lk(T().mu);
  Emit(kMark, std::string(s, n), {}, 0, nullptr, 0);
}

int open(const char* path, int flags, ...) {
  va_list ap;
  va_start(ap, flags);
  const mode_t mode = ModeArg(flags, ap);
  va_end(ap);
  static auto real = Real<int (*)(const char*, int, ...)>("open");
  if (!Enabled()) return real(path, flags, mode);
  // Normalise to openat semantics.
  static auto real_at = Real<int (*)(int, const char*, int, ...)>("openat");
  return DoOpen(real_at, AT_FDCWD, path, flags, mode);
}

int open64(const char* path, int flags, ...) {
  va_list ap;
  va_start(ap, flags);
  const mode_t mode = ModeArg(flags, ap);
  va_end(ap);
  static auto real_at = Real<int (*)(int, const char*, int, ...)>("openat64");
  return DoOpen(real_at, AT_FDCWD, path, flags, mode);
}

int openat(int dirfd, const char* path, int flags, ...) {
  va_list ap;
  va_start(ap, flags);
  const mode_t mode = ModeArg(flags, ap);
  va_end(ap);
  static auto real_at = Real<int (*)(int, const char*, int, ...)>("openat");
  return DoOpen(real_at, dirfd, path, flags, mode);
}

int openat64(int dirfd, const char* path, int flags, ...) {
  va_list ap;
  va_start(ap, flags);
  const mode_t mode = ModeArg(flags, ap);
  va_end(ap);
  static auto real_at = Real<int (*)(int, const char*, int, ...)>("openat64");
  return DoOpen(real_at, dirfd, path, flags, mode);
}

int close(int fd) {
  static auto real = Real<int (*)(int)>("close");
  if (!Enabled()) return real(fd);
  std::lock_guard lk(T().mu);
  T().fds.erase(fd);
  return real(fd);
}

ssize_t write(int fd, const void* buf, size_t n) {
  static auto real = Real<ssize_t (*)(int, const void*, size_t)>("write");
  if (!Enabled()) return real(fd, buf, n);
  return TracedWrite(fd, buf, -1, [&] { return real(fd, buf, n); });
}

ssize_t pwrite(int fd, const void* buf, size_t n, off_t off) {
  static auto real = Real<ssize_t (*)(int, const void*, size_t, off_t)>("pwrite");
  if (!Enabled()) return real(fd, buf, n, off);
  return TracedWrite(fd, buf, off, [&] { return real(fd, buf, n, off); });
}

ssize_t pwrite64(int fd, const void* buf, size_t n, off_t off) { return pwrite(fd, buf, n, off); }

ssize_t pwritev2(int fd, const struct iovec* iov, int cnt, off_t off, int flags) {
  static auto real = Real<ssize_t (*)(int, const struct iovec*, int, off_t, int)>("pwritev2");
  std::string rel;
  if (!Enabled()) return real(fd, iov, cnt, off, flags);
  std::lock_guard lk(T().mu);
  if (!TracedFd(fd, &rel)) return real(fd, iov, cnt, off, flags);
  const int64_t pos = off >= 0 ? off : static_cast<int64_t>(syscall(SYS_lseek, fd, 0, SEEK_CUR));
  const ssize_t r = real(fd, iov, cnt, off, flags);
  if (r > 0) {
    const std::string data = Gather(iov, cnt, static_cast<size_t>(r));
    Emit(kWrite, rel, {}, static_cast<uint64_t>(pos), data.data(), data.size());
  }
  return r;
}

ssize_t pwritev(int fd, const struct iovec* iov, int cnt, off_t off) {
  static auto real = Real<ssize_t (*)(int, const struct iovec*, int, off_t)>("pwritev");
  std::string rel;
  if (!Enabled()) return real(fd, iov, cnt, off);
  std::lock_guard lk(T().mu);
  if (!TracedFd(fd, &rel)) return real(fd, iov, cnt, off);
  const ssize_t r = real(fd, iov, cnt, off);
  if (r > 0) {
    const std::string data = Gather(iov, cnt, static_cast<size_t>(r));
    Emit(kWrite, rel, {}, static_cast<uint64_t>(off), data.data(), data.size());
  }
  return r;
}

int fsync(int fd) {
  static auto real = Real<int (*)(int)>("fsync");
  std::string rel;
  if (!Enabled()) return real(fd);
  std::lock_guard lk(T().mu);
  const int r = real(fd);
  if (r == 0 && TracedFd(fd, &rel)) Emit(kSync, rel, {}, 0, nullptr, 0);
  return r;
}

int fdatasync(int fd) {
  static auto real = Real<int (*)(int)>("fdatasync");
  std::string rel;
  if (!Enabled()) return real(fd);
  std::lock_guard lk(T().mu);
  const int r = real(fd);
  if (r == 0 && TracedFd(fd, &rel)) Emit(kSync, rel, {}, 0, nullptr, 0);
  return r;
}

int ftruncate(int fd, off_t len) {
  static auto real = Real<int (*)(int, off_t)>("ftruncate");
  std::string rel;
  if (!Enabled()) return real(fd, len);
  std::lock_guard lk(T().mu);
  const int r = real(fd, len);
  if (r == 0 && TracedFd(fd, &rel)) Emit(kTruncate, rel, {}, static_cast<uint64_t>(len), nullptr, 0);
  return r;
}

int fallocate(int fd, int mode, off_t off, off_t len) {
  static auto real = Real<int (*)(int, int, off_t, off_t)>("fallocate");
  std::string rel;
  if (!Enabled()) return real(fd, mode, off, len);
  std::lock_guard lk(T().mu);
  const int r = real(fd, mode, off, len);
  if (r == 0 && mode == 0 && TracedFd(fd, &rel)) {
    Emit(kExtend, rel, {}, static_cast<uint64_t>(off + len), nullptr, 0);
  }
  return r;
}

int fallocate64(int fd, int mode, off_t off, off_t len) { return fallocate(fd, mode, off, len); }

int rename(const char* a, const char* b) {
  static auto real = Real<int (*)(const char*, const char*)>("rename");
  return TracedPathOp(kRename, a, b, [&] { return real(a, b); });
}

int renameat2(int da, const char* a, int db, const char* b, unsigned flags) {
  static auto real = Real<int (*)(int, const char*, int, const char*, unsigned)>("renameat2");
  if (!(da == AT_FDCWD || a[0] == '/') || !(db == AT_FDCWD || b[0] == '/')) return real(da, a, db, b, flags);
  return TracedPathOp(kRename, a, b, [&] { return real(da, a, db, b, flags); });
}

int renameat(int da, const char* a, int db, const char* b) { return renameat2(da, a, db, b, 0); }

int unlink(const char* p) {
  static auto real = Real<int (*)(const char*)>("unlink");
  return TracedPathOp(kUnlink, p, nullptr, [&] { return real(p); });
}

int unlinkat(int dirfd, const char* p, int flags) {
  static auto real = Real<int (*)(int, const char*, int)>("unlinkat");
  if ((dirfd != AT_FDCWD && p[0] != '/') || (flags & AT_REMOVEDIR)) return real(dirfd, p, flags);
  return TracedPathOp(kUnlink, p, nullptr, [&] { return real(dirfd, p, flags); });
}

}  // extern "C"
