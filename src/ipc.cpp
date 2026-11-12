#include "vw/ipc.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace vw {
namespace {

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

}  // namespace

void UniqueFd::reset(int fd) noexcept {
  if (fd_ >= 0) ::close(fd_);
  fd_ = fd;
}

std::string unix_request(std::string_view path, std::string_view payload, int timeout_ms) {
  if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
    throw std::runtime_error("invalid Unix socket path");
  UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!fd) throw_errno("could not create Unix socket");
  if (timeout_ms > 0) {
    const timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    ::setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.data(), path.size());
  int connected;
  do {
    connected = ::connect(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address));
  } while (connected < 0 && errno == EINTR);
  if (connected < 0) throw_errno("Unix socket connect failed");

  for (std::size_t sent = 0; sent < payload.size();) {
    const ssize_t count = ::send(fd.get(), payload.data() + sent, payload.size() - sent,
                                 MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) throw_errno("Unix socket write failed");
    sent += static_cast<std::size_t>(count);
  }
  ::shutdown(fd.get(), SHUT_WR);

  std::string response;
  std::array<char, 4096> chunk{};
  for (;;) {
    const ssize_t count = ::read(fd.get(), chunk.data(), chunk.size());
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        throw std::runtime_error("Unix socket read timed out");
      throw_errno("Unix socket read failed");
    }
    response.append(chunk.data(), static_cast<std::size_t>(count));
  }
  return response;
}

}  // namespace vw
