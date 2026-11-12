// The deprecated Lua frontend. New programs link libvw and use vw::App.
#include "vw/app.hpp"
#include "vw/lua_config.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef VW_VERSION
#define VW_VERSION "unknown"
#endif

namespace {

constexpr std::string_view usage =
    "usage: vw [-c CONFIG] [--check] [--version] [--help]\n"
    "\n"
    "  -c, --config CONFIG  Lua configuration (default: $XDG_CONFIG_HOME/vw/init.lua)\n"
    "      --check          load the configuration, report the scene, and exit\n"
    "  -V, --version        print the version and exit\n"
    "  -h, --help           print this help and exit\n"
    "\n"
    "Send SIGHUP to force a reload; SIGINT or SIGTERM exits cleanly.\n"
    "\n"
    "Lua configurations are deprecated: write a C++ program against libvw\n"
    "(see examples/cpp). Set VW_NO_DEPRECATION_WARNING=1 to silence the notice.\n";

struct Options {
  std::filesystem::path config;
  bool check = false;
};

std::filesystem::path default_config() {
  if (const char* config_home = std::getenv("XDG_CONFIG_HOME");
      config_home != nullptr && *config_home == '/')
    return std::filesystem::path(config_home) / "vw/init.lua";
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0')
    throw std::runtime_error("HOME is not set; pass -c CONFIG");
  return std::filesystem::path(home) / ".config/vw/init.lua";
}

Options parse_options(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "-c" || argument == "--config") {
      if (index + 1 >= argc) throw std::runtime_error(std::string(argument) + " requires a path");
      options.config = argv[++index];
    } else if (argument.starts_with("--config=")) {
      options.config = argument.substr(std::string_view("--config=").size());
    } else if (argument == "--check") {
      options.check = true;
    } else if (argument == "-V" || argument == "--version") {
      std::cout << "vw " << VW_VERSION << '\n';
      std::exit(EXIT_SUCCESS);
    } else if (argument == "-h" || argument == "--help") {
      std::cout << usage;
      std::exit(EXIT_SUCCESS);
    } else {
      throw std::runtime_error("unknown argument: " + std::string(argument) + " (see --help)");
    }
  }
  if (options.config.empty()) options.config = default_config();
  std::error_code error;
  if (!std::filesystem::is_regular_file(options.config, error))
    throw std::runtime_error("configuration not found: " + options.config.string());
  return options;
}

std::string_view surface_kind(const vw::Scene& scene) {
  if (scene.session_lock) return "session lock";
  if (scene.toplevel) return "toplevel";
  return "layer surface";
}

void report(const std::filesystem::path& config, const vw::Scene& scene) {
  std::cout << config.string() << ": OK\n"
            << "  surface:      " << surface_kind(scene) << " " << scene.width << "x"
            << scene.height << " (namespace " << scene.surface_namespace << ")\n"
            << "  primitives:   " << scene.rectangles.size() << '\n'
            << "  rasters:      " << scene.rasters.size() << '\n'
            << "  interactions: " << scene.interactions.size() << '\n'
            << "  text fields:  " << scene.text_fields.size() << '\n'
            << "  scroll areas: " << scene.scroll_areas.size() << '\n';
}

void warn_deprecated() {
  if (const char* quiet = std::getenv("VW_NO_DEPRECATION_WARNING"); quiet != nullptr && *quiet != '\0')
    return;
  std::cerr << "vw: Lua configurations are deprecated; port to the C++ API (vw/app.hpp)\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_options(argc, argv);
    warn_deprecated();
    if (options.check) {
      report(options.config, vw::load_config(options.config));
      return EXIT_SUCCESS;
    }
    vw::LuaSource source(options.config);
    vw::run(source);
  } catch (const std::exception& error) {
    std::cerr << "vw: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
