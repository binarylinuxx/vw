#pragma once

#include <filesystem>

namespace vw::cli {

enum class InitKind {
  // CMakeLists.txt, main.cpp and .gitignore; configured and built with cmake.
  cmake,
  // The same plus flake.nix; locked, then built inside its dev shell.
  nix,
};

// Writes a new vw project into `directory` (created if missing) and builds
// it, printing each command it runs. Never overwrites a file. Returns the
// process exit status.
int init_project(const std::filesystem::path& directory, InitKind kind);

}  // namespace vw::cli
