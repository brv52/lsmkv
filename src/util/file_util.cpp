#include "util/file_util.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lsm {

void ScopedFd::Reset(int fd) noexcept {
  if (fd_ >= 0) ::close(fd_);
  fd_ = fd;
}

namespace {
std::string Numbered(const std::string& db, uint64_t number, const char* suffix) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/%06" PRIu64 "%s", number, suffix);
  return db + buf;
}
}  // namespace

std::string LogFileName(const std::string& db, uint64_t number) { return Numbered(db, number, ".log"); }
std::string TableFileName(const std::string& db, uint64_t number) { return Numbered(db, number, ".sst"); }
std::string ManifestFileName(const std::string& db, uint64_t number) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/MANIFEST-%06" PRIu64, number);
  return db + buf;
}
std::string CurrentFileName(const std::string& db) { return db + "/CURRENT"; }
std::string LockFileName(const std::string& db) { return db + "/LOCK"; }

bool ParseFileName(const std::string& name, uint64_t* number, FileType* type) {
  auto parse_num = [](const char* s, const char** end, uint64_t* out) {
    if (*s < '0' || *s > '9') return false;
    errno = 0;
    char* e = nullptr;
    const unsigned long long v = std::strtoull(s, &e, 10);
    if (errno != 0) return false;
    *out = v;
    *end = e;
    return true;
  };
  *number = 0;
  if (name == "CURRENT") { *type = FileType::kCurrent; return true; }
  if (name == "LOCK") { *type = FileType::kLock; return true; }
  if (name.rfind("MANIFEST-", 0) == 0) {
    const char* end;
    if (!parse_num(name.c_str() + 9, &end, number) || *end != '\0') return false;
    *type = FileType::kManifest;
    return true;
  }
  const char* end;
  if (!parse_num(name.c_str(), &end, number)) return false;
  const std::string suffix(end);
  if (suffix == ".log") *type = FileType::kLog;
  else if (suffix == ".sst") *type = FileType::kTable;
  else if (suffix == ".tmp") *type = FileType::kTemp;
  else return false;
  return true;
}

Status FsyncDir(const std::string& dir) {
  ScopedFd fd(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!fd.valid()) return Status::FromErrno("open dir " + dir);
  if (::fsync(fd.get()) != 0) return Status::FromErrno("fsync dir " + dir);
  return Status::OK();
}

Status CreateDirIfMissing(const std::string& dir) {
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return Status::FromErrno("mkdir " + dir);
  return Status::OK();
}

Status ListDir(const std::string& dir, std::vector<std::string>* names) {
  names->clear();
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return Status::FromErrno("opendir " + dir);
  while (dirent* e = ::readdir(d)) {
    if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
    names->emplace_back(e->d_name);
  }
  ::closedir(d);
  return Status::OK();
}

Status RemoveFile(const std::string& path) {
  if (::unlink(path.c_str()) != 0) return Status::FromErrno("unlink " + path);
  return Status::OK();
}

Status FileExists(const std::string& path, bool* exists) {
  struct stat st;
  if (::stat(path.c_str(), &st) == 0) {
    *exists = true;
    return Status::OK();
  }
  if (errno == ENOENT) {
    *exists = false;
    return Status::OK();
  }
  return Status::FromErrno("stat " + path);
}

Status GetFileSize(int fd, uint64_t* size) {
  struct stat st;
  if (::fstat(fd, &st) != 0) return Status::FromErrno("fstat");
  *size = static_cast<uint64_t>(st.st_size);
  return Status::OK();
}

Status ReadFileToString(const std::string& path, std::string* out) {
  out->clear();
  ScopedFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (!fd.valid()) {
    if (errno == ENOENT) return Status::NotFound(path);
    return Status::FromErrno("open " + path);
  }
  char buf[8192];
  for (;;) {
    const ssize_t n = ::read(fd.get(), buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::FromErrno("read " + path);
    }
    if (n == 0) break;
    out->append(buf, static_cast<size_t>(n));
  }
  return Status::OK();
}

Status WriteAll(int fd, const char* data, size_t n) {
  while (n > 0) {
    const ssize_t w = ::write(fd, data, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return Status::FromErrno("write");
    }
    data += w;
    n -= static_cast<size_t>(w);
  }
  return Status::OK();
}

Status WriteFileSync(const std::string& path, const Slice& data) {
  ScopedFd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
  if (!fd.valid()) return Status::FromErrno("open " + path);
  LSM_RETURN_IF_ERROR(WriteAll(fd.get(), data.data(), data.size()));
  if (::fsync(fd.get()) != 0) return Status::FromErrno("fsync " + path);
  return Status::OK();
}

Status LockDB(const std::string& db, ScopedFd* lock) {
  const std::string path = LockFileName(db);
  ScopedFd fd(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644));
  if (!fd.valid()) return Status::FromErrno("open " + path);
  if (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK) return Status::Busy("database is locked by another process: " + db);
    return Status::FromErrno("flock " + path);
  }
  *lock = std::move(fd);
  return Status::OK();
}

}  // namespace lsm
