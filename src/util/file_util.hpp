// POSIX file helpers and the database's file naming scheme.
//
// Only plain libc entry points (open/pwrite/fsync/fdatasync/renameat2/unlink)
// are used for durable state so the crash tracer and EIO shim can interpose
// on them with LD_PRELOAD.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "lsm/slice.hpp"
#include "lsm/status.hpp"

namespace lsm {

// RAII file descriptor.
class ScopedFd {
 public:
  ScopedFd() noexcept = default;
  explicit ScopedFd(int fd) noexcept : fd_(fd) {}
  ScopedFd(ScopedFd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
  ScopedFd& operator=(ScopedFd&& o) noexcept {
    if (this != &o) {
      Reset();
      fd_ = std::exchange(o.fd_, -1);
    }
    return *this;
  }
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  ~ScopedFd() { Reset(); }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  int release() noexcept { return std::exchange(fd_, -1); }
  void Reset(int fd = -1) noexcept;

 private:
  int fd_ = -1;
};

enum class FileType { kLog, kTable, kManifest, kCurrent, kTemp, kLock, kUnknown };

std::string LogFileName(const std::string& db, uint64_t number);
std::string TableFileName(const std::string& db, uint64_t number);
std::string ManifestFileName(const std::string& db, uint64_t number);
std::string CurrentFileName(const std::string& db);
std::string LockFileName(const std::string& db);
// Parses a bare file name (no directory). Returns false for foreign files.
bool ParseFileName(const std::string& name, uint64_t* number, FileType* type);

// fsync(2) a directory so that create/rename/unlink entries become durable.
Status FsyncDir(const std::string& dir);
Status CreateDirIfMissing(const std::string& dir);
Status ListDir(const std::string& dir, std::vector<std::string>* names);
Status RemoveFile(const std::string& path);
Status FileExists(const std::string& path, bool* exists);
Status GetFileSize(int fd, uint64_t* size);
Status ReadFileToString(const std::string& path, std::string* out);
// Writes `data` to `path` (O_TRUNC) and fsyncs it. Does NOT fsync the
// directory; callers that need the entry to be durable call FsyncDir.
Status WriteFileSync(const std::string& path, const Slice& data);
// Write every byte at the current position, retrying EINTR / short writes.
Status WriteAll(int fd, const char* data, size_t n);

// Exclusive advisory lock on <db>/LOCK held for the DB's lifetime.
Status LockDB(const std::string& db, ScopedFd* lock);

}  // namespace lsm
