// Error-carrying result type. Cheap to copy in the OK case (no allocation).
#pragma once

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace lsm {

class [[nodiscard]] Status {
 public:
  enum class Code : unsigned char {
    kOk = 0,
    kNotFound,
    kCorruption,
    kNotSupported,
    kInvalidArgument,
    kIOError,
    kBusy,
    // The engine hit an unrecoverable durability failure (fsync/fdatasync
    // error) and refuses all further mutations. See docs/design.md#fsyncgate.
    kPanic,
  };

  Status() noexcept = default;

  static Status OK() noexcept { return {}; }
  static Status NotFound(std::string_view msg = {}) { return {Code::kNotFound, msg, 0}; }
  static Status Corruption(std::string_view msg) { return {Code::kCorruption, msg, 0}; }
  static Status NotSupported(std::string_view msg) { return {Code::kNotSupported, msg, 0}; }
  static Status InvalidArgument(std::string_view msg) { return {Code::kInvalidArgument, msg, 0}; }
  static Status Busy(std::string_view msg) { return {Code::kBusy, msg, 0}; }
  static Status Panic(std::string_view msg, int err = 0) { return {Code::kPanic, msg, err}; }
  // `err` is a positive errno value.
  static Status IOError(std::string_view context, int err) { return {Code::kIOError, context, err}; }
  static Status FromErrno(std::string_view context) { return IOError(context, errno); }

  [[nodiscard]] bool ok() const noexcept { return code_ == Code::kOk; }
  [[nodiscard]] bool IsNotFound() const noexcept { return code_ == Code::kNotFound; }
  [[nodiscard]] bool IsCorruption() const noexcept { return code_ == Code::kCorruption; }
  [[nodiscard]] bool IsIOError() const noexcept { return code_ == Code::kIOError; }
  [[nodiscard]] bool IsNotSupported() const noexcept { return code_ == Code::kNotSupported; }
  [[nodiscard]] bool IsInvalidArgument() const noexcept { return code_ == Code::kInvalidArgument; }
  [[nodiscard]] bool IsPanic() const noexcept { return code_ == Code::kPanic; }
  [[nodiscard]] bool IsBusy() const noexcept { return code_ == Code::kBusy; }
  [[nodiscard]] Code code() const noexcept { return code_; }
  [[nodiscard]] int err() const noexcept { return errno_; }
  [[nodiscard]] std::string_view message() const noexcept {
    return msg_ ? std::string_view(*msg_) : std::string_view();
  }

  [[nodiscard]] std::string ToString() const {
    std::string r;
    switch (code_) {
      case Code::kOk: return "OK";
      case Code::kNotFound: r = "NotFound"; break;
      case Code::kCorruption: r = "Corruption"; break;
      case Code::kNotSupported: r = "NotSupported"; break;
      case Code::kInvalidArgument: r = "InvalidArgument"; break;
      case Code::kIOError: r = "IOError"; break;
      case Code::kBusy: r = "Busy"; break;
      case Code::kPanic: r = "Panic"; break;
    }
    if (msg_ && !msg_->empty()) {
      r += ": ";
      r += *msg_;
    }
    if (errno_ != 0) {
      r += " (errno ";
      r += std::to_string(errno_);
      r += ": ";
      r += std::strerror(errno_);
      r += ")";
    }
    return r;
  }

 private:
  Status(Code c, std::string_view msg, int err)
      : code_(c), errno_(err), msg_(msg.empty() ? nullptr : std::make_shared<const std::string>(msg)) {}

  Code code_ = Code::kOk;
  int errno_ = 0;
  std::shared_ptr<const std::string> msg_;
};

#define LSM_RETURN_IF_ERROR(expr)      \
  do {                                 \
    ::lsm::Status _s = (expr);         \
    if (!_s.ok()) return _s;           \
  } while (0)

}  // namespace lsm
