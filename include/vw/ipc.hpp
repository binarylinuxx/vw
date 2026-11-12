#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace vw {

// Owns a POSIX file descriptor and closes it on destruction.
class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() { reset(); }
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  int get() const noexcept { return fd_; }
  explicit operator bool() const noexcept { return fd_ >= 0; }
  int release() noexcept { return std::exchange(fd_, -1); }
  void reset(int fd = -1) noexcept;

 private:
  int fd_ = -1;
};

// Connects to a Unix stream socket, writes payload, half-closes, and reads the
// full response. A non-positive timeout disables socket timeouts.
std::string unix_request(std::string_view path, std::string_view payload, int timeout_ms = 1000);

}  // namespace vw
