#include "vw/hyprland.hpp"
#include "vw/ipc.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace vw::hyprland {
namespace {

constexpr int request_timeout_ms = 2000;

bool hyprland_session() {
  const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
  return signature != nullptr && *signature != '\0';
}

std::filesystem::path socket_path() {
  const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (signature == nullptr || *signature == '\0')
    throw std::runtime_error("HYPRLAND_INSTANCE_SIGNATURE is not set");
  std::error_code error;
  if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr) {
    auto path = std::filesystem::path(runtime) / "hypr" / signature / ".socket.sock";
    if (std::filesystem::exists(path, error)) return path;
  }
  auto legacy = std::filesystem::path("/tmp/hypr") / signature / ".socket.sock";
  if (std::filesystem::exists(legacy, error)) return legacy;
  throw std::runtime_error("Hyprland IPC socket was not found");
}

// Reads Hyprland's event socket on a worker thread. Each complete
// "name>>data" line updates the last event and wakes the main loop.
class EventSubscriber {
 public:
  EventSubscriber() : signal_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
    if (hyprland_session())
      thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
  }

  ~EventSubscriber() {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
  }

  int fd() const { return signal_.get(); }
  bool drain() {
    std::uint64_t count;
    bool signaled = false;
    while (read(signal_.get(), &count, sizeof(count)) > 0) signaled = true;
    return signaled;
  }
  Event last() {
    std::lock_guard lock(mutex_);
    return last_;
  }

 private:
  void run(const std::stop_token& stop) {
    while (!stop.stop_requested()) {
      try {
        listen(stop);
      } catch (...) {
      }
      for (int tick = 0; tick < 10 && !stop.stop_requested(); ++tick)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  void listen(const std::stop_token& stop) {
    const std::string path = (socket_path().parent_path() / ".socket2.sock").string();
    if (path.size() >= sizeof(sockaddr_un::sun_path))
      throw std::runtime_error("Hyprland event socket path is too long");
    UniqueFd fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd) throw std::runtime_error("event socket creation failed");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const timeval timeout{1, 0};
    setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
      throw std::runtime_error("event socket connection failed");
    std::string pending;
    std::array<char, 4096> chunk{};
    while (!stop.stop_requested()) {
      const ssize_t count = read(fd.get(), chunk.data(), chunk.size());
      if (count == 0) return;
      if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
        return;
      }
      pending.append(chunk.data(), static_cast<std::size_t>(count));
      bool received = false;
      for (std::size_t newline; (newline = pending.find('\n')) != std::string::npos;) {
        const std::string_view line(pending.data(), newline);
        const std::size_t separator = line.find(">>");
        {
          std::lock_guard lock(mutex_);
          last_.name = std::string(line.substr(0, separator));
          last_.data = separator == std::string_view::npos
              ? std::string{} : std::string(line.substr(separator + 2));
          ++last_.sequence;
        }
        pending.erase(0, newline + 1);
        received = true;
      }
      if (received) {
        const std::uint64_t one = 1;
        const ssize_t signaled = write(signal_.get(), &one, sizeof(one));
        (void)signaled;
      }
    }
  }

  UniqueFd signal_;
  std::mutex mutex_;
  Event last_;
  std::jthread thread_;
};

EventSubscriber& subscriber() {
  static EventSubscriber value;
  return value;
}

std::string trimmed(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
    text.pop_back();
  return text;
}

CommandResult command(std::string_view verb, std::string_view arguments) {
  std::string response = trimmed(request("/" + std::string(verb) + " " + std::string(arguments)));
  const bool ok = response == "ok";
  return {ok, std::move(response)};
}

}  // namespace

bool available() {
  try {
    (void)socket_path();
    return true;
  } catch (...) {
    return false;
  }
}

std::string request(std::string_view command) {
  return unix_request(socket_path().string(), command, request_timeout_ms);
}

nlohmann::json query(std::string_view endpoint) {
  return nlohmann::json::parse(request("j/" + std::string(endpoint)));
}

CommandResult dispatch(std::string_view arguments) { return command("dispatch", arguments); }
CommandResult keyword(std::string_view arguments) { return command("keyword", arguments); }

std::optional<Event> last_event() {
  Event event = subscriber().last();
  if (event.sequence == 0) return std::nullopt;
  return event;
}

int event_fd() { return subscriber().fd(); }
bool drain_events() { return subscriber().drain(); }

}  // namespace vw::hyprland
