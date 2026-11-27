#include "vws.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <atomic>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace vw::vws {
namespace {

constexpr std::array<std::uint8_t, 4> magic{'V', 'W', 'S', '1'};
constexpr std::uint32_t spirv_magic = 0x07230203U;
constexpr std::size_t max_params = 4;
constexpr std::size_t max_string = 255;

void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}
void put_string(std::vector<std::uint8_t>& out, std::string_view value) {
  if (value.size() > max_string) throw std::invalid_argument(std::format("'{}' is longer than 255 bytes", value));
  out.push_back(static_cast<std::uint8_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
  std::uint8_t u8() { return take(1)[0]; }
  std::uint16_t u16() {
    const auto b = take(2);
    return static_cast<std::uint16_t>(b[0] | b[1] << 8U);
  }
  std::uint32_t u32() {
    const auto b = take(4);
    return static_cast<std::uint32_t>(b[0]) | static_cast<std::uint32_t>(b[1]) << 8U |
           static_cast<std::uint32_t>(b[2]) << 16U | static_cast<std::uint32_t>(b[3]) << 24U;
  }
  std::string string() {
    const auto length = u8();
    const auto b = take(length);
    return {b.begin(), b.end()};
  }
  void align4() {
    while (position_ % 4 != 0) take(1);
  }
  std::span<const std::uint8_t> take(std::size_t count) {
    if (count > bytes_.size() - position_) throw std::runtime_error("VWS file is truncated");
    const auto result = bytes_.subspan(position_, count);
    position_ += count;
    return result;
  }
  std::size_t remaining() const { return bytes_.size() - position_; }

 private:
  std::span<const std::uint8_t> bytes_;
  std::size_t position_ = 0;
};

}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
  static const auto table = [] {
    std::array<std::uint32_t, 256> result{};
    for (std::uint32_t index = 0; index < 256; ++index) {
      std::uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) value = (value & 1U) != 0U ? 0xEDB88320U ^ (value >> 1U) : value >> 1U;
      result[index] = value;
    }
    return result;
  }();
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::uint8_t byte : bytes) crc = table[(crc ^ byte) & 0xFFU] ^ (crc >> 8U);
  return crc ^ 0xFFFFFFFFU;
}

bool is_vws(std::span<const std::uint8_t> bytes) {
  return bytes.size() >= magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin());
}

bool is_spirv(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 20 || bytes.size() % 4 != 0) return false;
  std::uint32_t first = 0;
  std::memcpy(&first, bytes.data(), 4);
  return first == spirv_magic;
}

std::vector<std::uint8_t> encode(const Shader& shader) {
  if (shader.params.size() > max_params) throw std::invalid_argument("a VWS shader names at most 4 parameters");
  if (shader.spirv.empty()) throw std::invalid_argument("a VWS shader needs SPIR-V");
  const std::span<const std::uint8_t> code(reinterpret_cast<const std::uint8_t*>(shader.spirv.data()),
                                           shader.spirv.size() * 4);
  std::vector<std::uint8_t> out(magic.begin(), magic.end());
  put16(out, format_version);
  put16(out, shader.flags);
  put32(out, static_cast<std::uint32_t>(shader.spirv.size()));
  put32(out, crc32(code));
  put_string(out, shader.name);
  out.push_back(static_cast<std::uint8_t>(shader.params.size()));
  for (const auto& param : shader.params) put_string(out, param);
  while (out.size() % 4 != 0) out.push_back(0);
  out.insert(out.end(), code.begin(), code.end());
  return out;
}

Shader decode(std::span<const std::uint8_t> bytes) {
  if (!is_vws(bytes)) throw std::runtime_error("not a VWS file (bad magic)");
  Reader reader(bytes);
  reader.take(magic.size());
  const std::uint16_t version = reader.u16();
  if (version != format_version)
    throw std::runtime_error(std::format("VWS format version {} is not supported (this vw reads {})", version,
                                         format_version));
  Shader shader;
  shader.flags = reader.u16();
  const std::uint32_t words = reader.u32();
  const std::uint32_t expected_crc = reader.u32();
  shader.name = reader.string();
  const std::uint8_t params = reader.u8();
  if (params > max_params) throw std::runtime_error("VWS file names more than 4 parameters");
  for (std::uint8_t index = 0; index < params; ++index) shader.params.push_back(reader.string());
  reader.align4();
  if (reader.remaining() != static_cast<std::size_t>(words) * 4)
    throw std::runtime_error("VWS file length does not match its SPIR-V size");
  const auto code = reader.take(static_cast<std::size_t>(words) * 4);
  if (crc32(code) != expected_crc) throw std::runtime_error("VWS checksum mismatch: the file is corrupt");
  if (!is_spirv(code)) throw std::runtime_error("VWS payload is not SPIR-V");
  shader.spirv.resize(words);
  std::memcpy(shader.spirv.data(), code.data(), code.size());
  return shader;
}

Interface inspect(std::span<const std::uint32_t> spirv) {
  Interface result;
  if (spirv.size() < 5 || spirv[0] != spirv_magic) return result;
  // Decorations come before the variables they name, so collect by id.
  struct Decorated {
    std::optional<std::uint32_t> set;
    std::optional<std::uint32_t> binding;
  };
  std::vector<std::pair<std::uint32_t, Decorated>> decorated;
  const auto entry = [&](std::uint32_t id) -> Decorated& {
    auto found = std::ranges::find(decorated, id, &std::pair<std::uint32_t, Decorated>::first);
    if (found == decorated.end()) {
      decorated.push_back({id, {}});
      return decorated.back().second;
    }
    return found->second;
  };
  std::optional<std::uint32_t> main_id;
  for (std::size_t at = 5; at < spirv.size();) {
    const std::uint32_t count = spirv[at] >> 16U;
    const std::uint32_t opcode = spirv[at] & 0xFFFFU;
    if (count == 0 || at + count > spirv.size()) break;
    const auto word = [&](std::size_t index) { return spirv[at + index]; };
    if (opcode == 15 && count >= 4 && word(1) == 5) {  // OpEntryPoint GLCompute
      const char* name = reinterpret_cast<const char*>(&spirv[at + 3]);
      const std::size_t room = (count - 3) * 4;
      if (std::string_view(name, strnlen(name, room)) == "main") {
        result.has_main = true;
        main_id = word(2);
      }
    } else if (opcode == 16 && count >= 6 && word(2) == 17) {  // OpExecutionMode LocalSize
      if (!main_id || word(1) == *main_id) {
        result.local_size[0] = word(3);
        result.local_size[1] = word(4);
        result.local_size[2] = word(5);
      }
    } else if (opcode == 71 && count >= 4) {  // OpDecorate
      if (word(2) == 33) entry(word(1)).binding = word(3);        // Binding
      else if (word(2) == 34) entry(word(1)).set = word(3);       // DescriptorSet
    }
    at += count;
  }
  for (const auto& [id, decoration] : decorated) {
    if (!decoration.binding) continue;
    const std::uint32_t set = decoration.set.value_or(0);
    if (set != 0) result.other_sets.push_back(set);
    else result.bindings.push_back(*decoration.binding);
  }
  std::ranges::sort(result.bindings);
  return result;
}

std::string validate(const Interface& interface, std::uint16_t& flags) {
  if (!interface.has_main) return "no GLCompute entry point named main";
  if (interface.local_size[0] != 8 || interface.local_size[1] != 8 || interface.local_size[2] != 1)
    return std::format("local size is {}x{}x{}; vw dispatches 8x8x1 workgroups (layout(local_size_x = 8, "
                       "local_size_y = 8) in;)",
                       interface.local_size[0], interface.local_size[1], interface.local_size[2]);
  if (!interface.other_sets.empty()) return "only descriptor set 0 exists";
  bool target = false;
  flags = 0;
  for (std::uint32_t binding : interface.bindings) {
    if (binding == 0) target = true;
    else if (binding == 1) flags |= reads_data;
    else if (binding == 2) flags |= reads_backdrop;
    else if (binding == 3) flags |= uses_targets | reads_data;
    // A pass writing a named image (binding 4) draws there, not on the surface.
    else if (binding == 4) target = true, flags |= uses_targets | reads_data;
    else return std::format("binding {} does not exist; vw provides 0 (target), 1 (data), 2 (backdrop), "
                            "3 and 4 (pass images)", binding);
  }
  if (!target)
    return "neither binding 0 (the target image) nor 4 (a pass's named image) is declared, so the shader cannot draw";
  return {};
}

namespace {

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r");
  return value.substr(first, last - first + 1);
}

// "// @vw name: X" and "// @vw params: a, b, c" lines.
void read_metadata(const std::string& source, Shader& shader) {
  std::istringstream lines(source);
  for (std::string line; std::getline(lines, line);) {
    const auto at = line.find("@vw ");
    if (at == std::string::npos || line.find("//") > at) continue;
    const std::string rest = line.substr(at + 4);
    const auto colon = rest.find(':');
    if (colon == std::string::npos) continue;
    const std::string key = trim(rest.substr(0, colon));
    const std::string value = trim(rest.substr(colon + 1));
    if (key == "name") {
      shader.name = value;
    } else if (key == "params") {
      shader.params.clear();
      std::istringstream names(value);
      for (std::string name; std::getline(names, name, ',');)
        if (!trim(name).empty()) shader.params.push_back(trim(name));
      if (shader.params.size() > max_params)
        throw std::runtime_error("'@vw params' names " + std::to_string(shader.params.size()) +
                                 " parameters; shader_params holds 4");
    }
  }
}

// `#define VW_USE_TIME` on a line of its own, as the other VW_USE_ switches.
bool uses_time(const std::string& source) {
  std::istringstream lines(source);
  for (std::string line; std::getline(lines, line);) {
    std::istringstream words(line);
    std::string directive, name;
    words >> directive >> name;
    if ((directive == "#define" && name == "VW_USE_TIME") ||
        (directive == "#" && name == "define" && (words >> name) && name == "VW_USE_TIME"))
      return true;
  }
  return false;
}

int run(const std::vector<std::string>& argv) {
  std::vector<char*> arguments;
  for (const auto& argument : argv) arguments.push_back(const_cast<char*>(argument.c_str()));
  arguments.push_back(nullptr);
  const pid_t child = fork();
  if (child < 0) throw std::runtime_error("cannot start " + argv.front());
  if (child == 0) {
    execvp(arguments.front(), arguments.data());
    std::fprintf(stderr, "vw: cannot run %s\n", arguments.front());
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

}  // namespace

bool is_glsl(std::span<const std::uint8_t> bytes) {
  return !bytes.empty() && !is_vws(bytes) && !is_spirv(bytes);
}

Shader compile(const std::string& source_path, const CompileOptions& options) {
  const std::filesystem::path input(source_path);
  Shader shader;
  shader.name = input.stem().string();
  const auto bytes = read_bytes(input);
  const std::string source(bytes.begin(), bytes.end());
  read_metadata(source, shader);
  if (!options.name.empty()) shader.name = options.name;

  std::string glslc = options.glslc;
  if (glslc.empty()) {
    const char* environment = std::getenv("VW_GLSLC");
    glslc = environment != nullptr && *environment != '\0' ? environment : "glslc";
  }
  // Beside the system's temporary files, unique per process and call.
  static std::atomic<unsigned> counter{0};
  const std::filesystem::path compiled = std::filesystem::temp_directory_path() /
      std::format("vw-shader-{}-{}.spv", getpid(), counter++);
  std::vector<std::string> command{glslc, "-fshader-stage=compute", "--target-env=vulkan1.0", "-O"};
  for (const auto& directory : options.includes) command.push_back("-I" + directory);
#ifdef VW_SHADER_INCLUDE_DIRS
  // vw's own headers, so `#include <vw/shader.glsl>` always resolves.
  std::istringstream builtin(VW_SHADER_INCLUDE_DIRS);
  for (std::string directory; std::getline(builtin, directory, ':');)
    if (!directory.empty()) command.push_back("-I" + directory);
#endif
  for (const auto& define : options.defines) command.push_back("-D" + define);
  if (!options.depfile.empty())
    command.insert(command.end(), {"-MD", "-MF", options.depfile, "-MT", options.depfile_target});
  command.insert(command.end(), {input.string(), "-o", compiled.string()});
  const int status = run(command);
  std::error_code ignored;
  if (status != 0) {
    std::filesystem::remove(compiled, ignored);
    throw std::runtime_error(std::format("glslc failed for {}", input.string()));
  }
  const auto code = read_bytes(compiled);
  std::filesystem::remove(compiled, ignored);
  if (!is_spirv(code)) throw std::runtime_error("glslc did not produce SPIR-V");
  shader.spirv.resize(code.size() / 4);
  std::memcpy(shader.spirv.data(), code.data(), code.size());

  const std::string problem = validate(inspect(shader.spirv), shader.flags);
  if (!problem.empty()) throw std::runtime_error(input.string() + ": " + problem);
  if (uses_time(source)) shader.flags |= animated;
  return shader;
}

std::uint16_t peek_flags(const std::string& path) {
  struct Entry {
    std::filesystem::file_time_type modified;
    std::uint16_t flags = 0;
  };
  static std::mutex mutex;
  static std::unordered_map<std::string, Entry> cache;
  std::error_code error;
  const auto modified = std::filesystem::last_write_time(path, error);
  if (error) return 0;
  const std::lock_guard lock(mutex);
  if (const auto found = cache.find(path); found != cache.end() && found->second.modified == modified)
    return found->second.flags;
  std::uint16_t flags = 0;
  try {
    const auto bytes = read_bytes(path);
    if (is_vws(bytes)) {
      flags = decode(bytes).flags;
    } else if (is_spirv(bytes)) {
      flags = reads_backdrop | reads_data;
    } else {
      const std::string source(bytes.begin(), bytes.end());
      if (uses_time(source)) flags |= animated;
    }
  } catch (const std::exception&) {
    flags = 0;
  }
  cache[path] = Entry{modified, flags};
  return flags;
}

}  // namespace vw::vws
