// Non-owning view over a contiguous byte range.
#pragma once

#include <cassert>
#include <compare>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace lsm {

class Slice {
 public:
  constexpr Slice() noexcept = default;
  constexpr Slice(const char* d, size_t n) noexcept : data_(d), size_(n) {}
  Slice(const std::string& s) noexcept : data_(s.data()), size_(s.size()) {}  // NOLINT
  constexpr Slice(std::string_view s) noexcept : data_(s.data()), size_(s.size()) {}  // NOLINT
  Slice(const char* s) noexcept : data_(s), size_(std::strlen(s)) {}  // NOLINT

  [[nodiscard]] constexpr const char* data() const noexcept { return data_; }
  [[nodiscard]] constexpr size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

  constexpr char operator[](size_t i) const noexcept {
    assert(i < size_);
    return data_[i];
  }

  constexpr void remove_prefix(size_t n) noexcept {
    assert(n <= size_);
    data_ += n;
    size_ -= n;
  }

  constexpr void clear() noexcept {
    data_ = "";
    size_ = 0;
  }

  [[nodiscard]] std::string ToString() const { return {data_, size_}; }
  [[nodiscard]] constexpr std::string_view view() const noexcept { return {data_, size_}; }

  // Three-way lexicographic byte comparison (<0, 0, >0).
  [[nodiscard]] int compare(const Slice& b) const noexcept {
    const size_t min_len = size_ < b.size_ ? size_ : b.size_;
    int r = min_len == 0 ? 0 : std::memcmp(data_, b.data_, min_len);
    if (r == 0) {
      if (size_ < b.size_) r = -1;
      else if (size_ > b.size_) r = 1;
    }
    return r;
  }

  [[nodiscard]] bool starts_with(const Slice& x) const noexcept {
    return size_ >= x.size_ && (x.size_ == 0 || std::memcmp(data_, x.data_, x.size_) == 0);
  }

  friend bool operator==(const Slice& a, const Slice& b) noexcept {
    return a.size_ == b.size_ && (a.size_ == 0 || std::memcmp(a.data_, b.data_, a.size_) == 0);
  }
  friend std::strong_ordering operator<=>(const Slice& a, const Slice& b) noexcept {
    const int r = a.compare(b);
    return r < 0 ? std::strong_ordering::less
                 : (r > 0 ? std::strong_ordering::greater : std::strong_ordering::equal);
  }

 private:
  const char* data_ = "";
  size_t size_ = 0;
};

}  // namespace lsm
