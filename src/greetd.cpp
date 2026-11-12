#include "vw/greetd.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace vw::greetd {
namespace {

using Json = nlohmann::json;

// What each request in flight was, so its reply is read in context. greetd
// answers requests in order, one reply each.
enum class Request : std::uint8_t { create, answer, start, cancel, recover };

class Client {
 public:
  ~Client() {
    if (fd_ >= 0) close(fd_);
  }

  bool connected() {
    if (fd_ >= 0) return true;
    if (tried_) return false;
    tried_ = true;
    const char* path = std::getenv("GREETD_SOCK");
    if (path == nullptr || *path == '\0') return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (std::strlen(path) >= sizeof(address.sun_path)) return false;
    std::strcpy(address.sun_path, path);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
      close(fd);
      return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fd_ = fd;
    return true;
  }

  int fd() { return connected() ? fd_ : -1; }
  const State& state() const { return state_; }

  void begin(std::string_view username) {
    require();
    // A login already under way is cancelled first.
    if (state_.stage != Stage::idle || !pending_.empty()) send({{"type", "cancel_session"}}, Request::cancel);
    state_ = State{};
    state_.username = username;
    state_.stage = Stage::waiting;
    send({{"type", "create_session"}, {"username", std::string(username)}}, Request::create);
  }

  void answer(std::string_view response) {
    require();
    if (state_.stage != Stage::prompting) throw std::logic_error("greetd is not asking anything");
    // Info and error prompts are acknowledged without an answer.
    const bool question = state_.prompt == Prompt::visible || state_.prompt == Prompt::secret;
    Json message{{"type", "post_auth_message_response"}};
    message["response"] = question ? Json(std::string(response)) : Json(nullptr);
    state_.stage = Stage::waiting;
    send(message, Request::answer);
  }

  void start(const std::vector<std::string>& command, const std::vector<std::string>& environment) {
    require();
    if (state_.stage != Stage::authenticated) throw std::logic_error("the login is not authenticated yet");
    if (command.empty()) throw std::invalid_argument("a session needs a command");
    state_.stage = Stage::waiting;
    send({{"type", "start_session"}, {"cmd", command}, {"env", environment}}, Request::start);
  }

  void cancel() {
    require();
    send({{"type", "cancel_session"}}, Request::cancel);
  }

  // Reads every reply that has arrived; true when the state changed.
  bool drain() {
    if (fd_ < 0) return false;
    char chunk[4096];
    for (;;) {
      const ssize_t count = read(fd_, chunk, sizeof(chunk));
      if (count > 0) {
        input_.append(chunk, static_cast<std::size_t>(count));
        continue;
      }
      if (count == 0) {
        lost("greetd closed the connection");
        return true;
      }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      lost(std::string("greetd connection failed: ") + std::strerror(errno));
      return true;
    }
    // Replies to cancels sent on the greeter's behalf change nothing it shows.
    const State before = state_;
    while (input_.size() >= sizeof(std::uint32_t)) {
      std::uint32_t length = 0;
      std::memcpy(&length, input_.data(), sizeof(length));  // native byte order
      if (input_.size() < sizeof(length) + length) break;
      const std::string body = input_.substr(sizeof(length), length);
      input_.erase(0, sizeof(length) + length);
      reply(Json::parse(body, nullptr, false));
    }
    return state_.stage != before.stage || state_.prompt != before.prompt || state_.message != before.message ||
           state_.denied != before.denied;
  }

 private:
  void require() {
    if (!connected()) throw std::runtime_error("greetd is not running here ($GREETD_SOCK is not set or answers nothing)");
  }

  void send(const Json& message, Request request) {
    const std::string body = message.dump();
    const auto length = static_cast<std::uint32_t>(body.size());
    std::string frame(sizeof(length), '\0');
    std::memcpy(frame.data(), &length, sizeof(length));
    frame += body;
    std::string_view rest(frame);
    while (!rest.empty()) {
      const ssize_t written = write(fd_, rest.data(), rest.size());
      if (written > 0) {
        rest.remove_prefix(static_cast<std::size_t>(written));
      } else if (written < 0 && (errno == EAGAIN || errno == EINTR)) {
        pollfd ready{fd_, POLLOUT, 0};
        (void)poll(&ready, 1, 1000);
      } else {
        lost("could not write to greetd");
        throw std::runtime_error(state_.message);
      }
    }
    pending_.push_back(request);
  }

  void reply(const Json& message) {
    if (pending_.empty() || !message.is_object()) return;
    const Request request = pending_.front();
    pending_.pop_front();
    const std::string type = message.value("type", "");
    if (request == Request::cancel || request == Request::recover) {
      // A cancel after a failure keeps the failure on show.
      if (request == Request::cancel && pending_.empty()) state_ = State{};
      return;
    }
    if (type == "success") {
      state_.prompt = Prompt::none;
      state_.message.clear();
      state_.stage = request == Request::start ? Stage::started : Stage::authenticated;
    } else if (type == "auth_message") {
      static const std::map<std::string, Prompt> prompts{
          {"visible", Prompt::visible}, {"secret", Prompt::secret}, {"info", Prompt::info}, {"error", Prompt::error}};
      const auto found = prompts.find(message.value("auth_message_type", ""));
      state_.stage = Stage::prompting;
      state_.prompt = found == prompts.end() ? Prompt::info : found->second;
      state_.message = message.value("auth_message", "");
    } else {
      // An error ends this login; greetd wants it cancelled before the next.
      state_.stage = Stage::failed;
      state_.prompt = Prompt::none;
      state_.denied = message.value("error_type", "") == "auth_error";
      state_.message = message.value("description", "the login failed");
      send({{"type", "cancel_session"}}, Request::recover);
    }
  }

  void lost(std::string why) {
    close(fd_);
    fd_ = -1;
    pending_.clear();
    input_.clear();
    state_.stage = Stage::failed;
    state_.prompt = Prompt::none;
    state_.message = std::move(why);
  }

  int fd_ = -1;
  bool tried_ = false;
  std::string input_;
  std::deque<Request> pending_;
  State state_;
};

Client& client() {
  static Client instance;
  return instance;
}

// KEY=value from /etc/login.defs, or `fallback`.
std::uint32_t login_def(const std::string& key, std::uint32_t fallback) {
  std::ifstream file("/etc/login.defs");
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream words(line);
    std::string name;
    std::uint32_t value = 0;
    if (words >> name && name == key && words >> value) return value;
  }
  return fallback;
}

std::vector<std::string> split(const std::string& text, char separator) {
  std::vector<std::string> parts;
  std::string part;
  std::istringstream stream(text);
  while (std::getline(stream, part, separator)) parts.push_back(part);
  if (!text.empty() && text.back() == separator) parts.emplace_back();
  return parts;
}

// Exec= into words: double quotes group, backslash escapes, %f-style field
// codes (meaningless for a session) are dropped.
std::vector<std::string> exec_words(const std::string& exec) {
  std::vector<std::string> words;
  std::string word;
  bool quoted = false;
  bool any = false;
  for (std::size_t index = 0; index < exec.size(); ++index) {
    const char c = exec[index];
    if (c == '\\' && index + 1 < exec.size()) {
      word += exec[++index];
      any = true;
    } else if (c == '"') {
      quoted = !quoted;
      any = true;
    } else if ((c == ' ' || c == '\t') && !quoted) {
      if (any) words.push_back(std::move(word));
      word.clear();
      any = false;
    } else {
      word += c;
      any = true;
    }
  }
  if (any) words.push_back(std::move(word));
  std::erase_if(words, [](const std::string& w) { return w.size() == 2 && w[0] == '%'; });
  return words;
}

}  // namespace

bool available() { return client().connected(); }
State state() { return client().state(); }
void begin(std::string_view username) { client().begin(username); }
void answer(std::string_view response) { client().answer(response); }
void start(const std::vector<std::string>& command, const std::vector<std::string>& environment) {
  client().start(command, environment);
}
void cancel() { client().cancel(); }
int event_fd() { return client().fd(); }
bool drain_events() {
  try {
    return client().drain();
  } catch (...) {
    return true;
  }
}

std::vector<User> users() {
  const std::uint32_t lowest = login_def("UID_MIN", 1000);
  const std::uint32_t highest = login_def("UID_MAX", 60000);
  std::vector<User> result;
  std::ifstream file("/etc/passwd");
  std::string line;
  while (std::getline(file, line)) {
    const auto fields = split(line, ':');
    if (fields.size() < 7) continue;
    std::uint32_t uid = 0;
    try {
      uid = static_cast<std::uint32_t>(std::stoul(fields[2]));
    } catch (...) {
      continue;
    }
    const std::string& shell = fields[6];
    if (uid < lowest || uid > highest || shell.ends_with("nologin") || shell.ends_with("false")) continue;
    result.push_back({fields[0], fields[4].substr(0, fields[4].find(',')), fields[5], uid});
  }
  std::ranges::sort(result, {}, &User::uid);
  return result;
}

std::vector<Session> sessions() {
  std::string directories = std::getenv("XDG_DATA_DIRS") != nullptr ? std::getenv("XDG_DATA_DIRS") : "";
  if (directories.empty()) directories = "/usr/local/share:/usr/share";
  std::vector<Session> result;
  std::set<std::string> seen;  // the first file of a name wins, as in XDG
  for (const auto& [kind, wayland] : {std::pair{"wayland-sessions", true}, std::pair{"xsessions", false}}) {
    for (const auto& directory : split(directories, ':')) {
      std::error_code error;
      const std::filesystem::path folder = std::filesystem::path(directory) / kind;
      std::vector<std::filesystem::path> files;
      for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        if (entry.path().extension() == ".desktop") files.push_back(entry.path());
      std::ranges::sort(files);
      for (const auto& path : files) {
        if (!seen.insert(std::string(kind) + "/" + path.filename().string()).second) continue;
        std::ifstream file(path);
        std::string line;
        std::string section;
        Session session{.name = path.stem().string(), .command = {}, .file = path.string(), .wayland = wayland};
        bool hidden = false;
        while (std::getline(file, line)) {
          if (line.starts_with('[')) {
            section = line;
            continue;
          }
          if (section != "[Desktop Entry]") continue;
          if (line.starts_with("Name=")) session.name = line.substr(5);
          else if (line.starts_with("Exec=")) session.command = exec_words(line.substr(5));
          else if (line == "Hidden=true" || line == "NoDisplay=true") hidden = true;
        }
        if (!hidden && !session.command.empty()) result.push_back(std::move(session));
      }
    }
  }
  return result;
}

}  // namespace vw::greetd
