#include "vw/process.hpp"
#include "vw/ipc.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace vw {
namespace {

struct PipeCloser {
  void operator()(FILE* pipe) const { if (pipe != nullptr) (void)pclose(pipe); }
};

std::string run_command(const std::string& command) {
  std::unique_ptr<FILE, PipeCloser> pipe(popen(command.c_str(), "re"));
  if (!pipe) throw std::system_error(errno, std::generic_category(),
                                     "could not start poll command");
  std::string output;
  std::array<char, 4096> chunk{};
  for (std::size_t count; (count = std::fread(chunk.data(), 1, chunk.size(), pipe.get())) > 0;)
    output.append(chunk.data(), count);
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
    output.pop_back();
  return output;
}

struct PollEntry {
  std::string output;
  std::chrono::steady_clock::time_point next{};
  bool ready = false;
  bool running = false;
};

struct PollService {
  std::mutex mutex;
  std::unordered_map<std::string, PollEntry> entries;
  UniqueFd signal{eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};
};

// Worker threads share ownership so a late completion never touches a
// destroyed service during process exit.
const std::shared_ptr<PollService>& poll_service() {
  static const auto service = std::make_shared<PollService>();
  return service;
}

}  // namespace

pid_t spawn(const std::vector<std::string>& arguments) {
  if (arguments.empty() || arguments.front().empty())
    throw std::invalid_argument("spawn requires a command");
  std::vector<std::string> storage = arguments;
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& argument : storage) argv.push_back(argument.data());
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  sigset_t mask;
  sigemptyset(&mask);
  posix_spawnattr_setsigmask(&attributes, &mask);
  sigset_t defaults;
  sigemptyset(&defaults);
  for (int number : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE, SIGCHLD}) sigaddset(&defaults, number);
  posix_spawnattr_setsigdefault(&attributes, &defaults);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
                                            POSIX_SPAWN_SETSID);
  pid_t pid = 0;
  const int result = posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environ);
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  if (result != 0)
    throw std::system_error(result, std::generic_category(), "could not spawn " + arguments[0]);
  std::thread([pid] {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  }).detach();
  return pid;
}

pid_t spawn_shell(std::string_view command) {
  return spawn({"/bin/sh", "-c", std::string(command)});
}

std::string poll_command(const std::string& command, double interval) {
  if (!std::isfinite(interval) || interval <= 0.0)
    throw std::invalid_argument("poll interval must be positive and finite");
  const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(interval));
  const auto& service = poll_service();
  std::unique_lock lock(service->mutex);
  PollEntry& entry = service->entries[command];
  const auto now = std::chrono::steady_clock::now();
  if (!entry.ready) {
    lock.unlock();
    std::string first = run_command(command);
    lock.lock();
    entry.output = std::move(first);
    entry.ready = true;
    entry.next = now + period;
  } else if (!entry.running && entry.next <= now) {
    entry.running = true;
    std::thread([service = service, command, period] {
      std::optional<std::string> result;
      try { result = run_command(command); } catch (...) {}
      bool changed = false;
      {
        std::lock_guard guard(service->mutex);
        PollEntry& done = service->entries[command];
        if (result && *result != done.output) {
          done.output = std::move(*result);
          changed = true;
        }
        done.running = false;
        done.next = std::chrono::steady_clock::now() + period;
      }
      if (changed) {
        const std::uint64_t one = 1;
        const ssize_t signaled = write(service->signal.get(), &one, sizeof(one));
        (void)signaled;
      }
    }).detach();
  }
  return entry.output;
}

int poll_event_fd() { return poll_service()->signal.get(); }

bool drain_poll_events() {
  std::uint64_t count = 0;
  bool signaled = false;
  while (read(poll_service()->signal.get(), &count, sizeof(count)) > 0) signaled = true;
  return signaled;
}

}  // namespace vw
