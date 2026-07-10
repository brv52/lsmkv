// Shared helpers for unit tests.
#pragma once

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "lsm/iobackend.hpp"
#include "lsm/options.hpp"

namespace lsm::test {

// mkdtemp() under $LSMKV_TEST_DIR (default: system temp dir), removed on
// destruction. Point LSMKV_TEST_DIR at an NVMe/ext4/xfs mount to exercise
// real O_DIRECT; tmpfs falls back to buffered I/O.
class TempDir {
 public:
  TempDir() {
    const char* base = std::getenv("LSMKV_TEST_DIR");
    std::string tmpl = (base ? std::string(base) : std::filesystem::temp_directory_path().string()) +
                       "/lsmkv_test_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (::mkdtemp(buf.data()) == nullptr) std::abort();
    path_ = buf.data();
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::string Sub(const std::string& name) const { return path_ + "/" + name; }

 private:
  std::string path_;
};

inline const char* BackendName(IoBackendKind k) {
  switch (k) {
    case IoBackendKind::kAuto: return "Auto";
    case IoBackendKind::kUring: return "Uring";
    case IoBackendKind::kPread: return "Pread";
  }
  return "?";
}

// Returns the backend or nullptr if unsupported here (tests then skip).
inline std::shared_ptr<IoBackend> MakeBackend(IoBackendKind k, const Options& base = Options()) {
  Options o = base;
  o.io_backend = k;
  Status s;
  return NewIoBackend(o, &s);
}

}  // namespace lsm::test
