#include "init.hpp"

#include "vw_project_template.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vw::cli {
namespace {

namespace fs = std::filesystem;

// The template's placeholder project name.
constexpr std::string_view placeholder = "my-widget";

// A CMake target and package name from the directory name.
std::string project_name(const fs::path& directory) {
  std::string name;
  for (const char character : fs::absolute(directory).lexically_normal().filename().string())
    name += std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' ||
                    character == '-'
                ? character
                : '-';
  if (name.empty() || std::isalpha(static_cast<unsigned char>(name.front())) == 0)
    return std::string(placeholder);
  return name;
}

std::string named(std::string_view text, const std::string& name) {
  std::string result(text);
  for (auto at = result.find(placeholder); at != std::string::npos;
       at = result.find(placeholder, at + name.size()))
    result.replace(at, placeholder.size(), name);
  return result;
}

bool on_path(std::string_view program) {
  const char* path = std::getenv("PATH");
  if (path == nullptr) return false;
  std::string_view rest(path);
  while (!rest.empty()) {
    const auto colon = rest.find(':');
    const fs::path candidate = fs::path(rest.substr(0, colon)) / program;
    if (access(candidate.c_str(), X_OK) == 0) return true;
    if (colon == std::string_view::npos) break;
    rest.remove_prefix(colon + 1);
  }
  return false;
}

// Runs a command in `directory`, showing it first, and waits for it.
bool run(const fs::path& directory, const std::vector<std::string>& command) {
  std::cout << "$";
  for (const auto& part : command)
    std::cout << ' ' << (part.find(' ') == std::string::npos ? part : '"' + part + '"');
  std::cout << std::endl;
  std::vector<char*> arguments;
  for (const auto& part : command) arguments.push_back(const_cast<char*>(part.c_str()));
  arguments.push_back(nullptr);
  const pid_t child = fork();
  if (child < 0) return false;
  if (child == 0) {
    if (chdir(directory.c_str()) != 0) _exit(127);
    execvp(arguments.front(), arguments.data());
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0)
    if (errno != EINTR) return false;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Where this vw is installed, when its CMake package sits beside it; lets
// find_package(vw) work for an install outside the default prefixes.
std::optional<fs::path> install_prefix() {
  std::error_code error;
  const fs::path executable = fs::read_symlink("/proc/self/exe", error);
  if (error) return std::nullopt;
  const fs::path prefix = executable.parent_path().parent_path();
  for (const char* lib : {"lib", "lib64"})
    if (fs::exists(prefix / lib / "cmake/vw/vwConfig.cmake", error)) return prefix;
  return std::nullopt;
}

std::vector<std::string> cmake_configure() {
  std::vector<std::string> command{"cmake", "-S", ".", "-B", "build"};
  if (on_path("ninja")) command.insert(command.end(), {"-G", "Ninja"});
  if (on_path("clang++")) command.emplace_back("-DCMAKE_CXX_COMPILER=clang++");
  if (const auto prefix = install_prefix()) command.push_back("-DCMAKE_PREFIX_PATH=" + prefix->string());
  return command;
}

int fail(std::string_view message) {
  std::cerr << "vw: " << message << '\n';
  return EXIT_FAILURE;
}

}  // namespace

int init_project(const fs::path& directory, InitKind kind) {
  const std::string name = project_name(directory);
  std::vector<std::pair<std::string, std::string>> files{
      {"CMakeLists.txt", named(project_template::cmakelists, name)},
      {"main.cpp", std::string(project_template::main_cpp)},
      {".gitignore", std::string(project_template::gitignore)},
  };
  if (kind == InitKind::nix) files.emplace_back("flake.nix", named(project_template::flake_nix, name));

  for (const char* tool : kind == InitKind::nix ? std::vector{"nix", "git"} : std::vector{"cmake"})
    if (!on_path(tool)) return fail(std::string(tool) + " is not installed");

  std::error_code error;
  fs::create_directories(directory, error);
  if (error) return fail("could not create " + directory.string() + ": " + error.message());
  for (const auto& [file, content] : files)
    if (fs::exists(directory / file, error))
      return fail(directory.string() + "/" + file + " already exists; nothing was written");
  for (const auto& [file, content] : files) {
    std::ofstream output(directory / file, std::ios::binary);
    output << content;
    if (!output) return fail("could not write " + (directory / file).string());
    std::cout << "wrote " << (directory / file).string() << '\n';
  }

  if (kind == InitKind::cmake) {
    if (!run(directory, cmake_configure())) return fail("cmake could not configure the project");
    if (!run(directory, {"cmake", "--build", "build"})) return fail("the project did not build");
    std::cout << "\nReady. Run it with: " << (directory / "build" / name).string() << '\n';
    return EXIT_SUCCESS;
  }

  // Flakes only see files git tracks.
  const std::vector<std::string> nix{"nix", "--extra-experimental-features", "nix-command flakes"};
  const auto with_nix = [&](std::vector<std::string> rest) {
    std::vector<std::string> command = nix;
    command.insert(command.end(), rest.begin(), rest.end());
    return command;
  };
  if (!fs::exists(directory / ".git", error) && !run(directory, {"git", "init", "-q"}))
    return fail("git init failed");
  if (!run(directory, {"git", "add", "-A"})) return fail("git add failed");
  if (!run(directory, with_nix({"flake", "lock"}))) return fail("nix flake lock failed");
  if (!run(directory, with_nix({"develop", "--command", "cmake", "-S", ".", "-B", "build", "-G", "Ninja"})))
    return fail("cmake could not configure the project");
  if (!run(directory, with_nix({"develop", "--command", "cmake", "--build", "build"})))
    return fail("the project did not build");
  std::cout << "\nReady. Run it with: " << (directory / "build" / name).string()
            << "\n  or: nix run, from " << directory.string() << '\n';
  return EXIT_SUCCESS;
}

}  // namespace vw::cli
