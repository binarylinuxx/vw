// vw-shaderc: compiles GLSL compute shaders into vw's .vws format.
//
//   vw-shaderc glass.comp -o glass.vws            GLSL (through glslc)
//   vw-shaderc glass.spv -o glass.vws             wrap existing SPIR-V
//   vw-shaderc --inspect glass.vws                print what a .vws holds
//
// Metadata comes from comments in the source:
//   // @vw name: Liquid glass
//   // @vw params: depth, strength, shift, tint
#include "../src/vws.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace vws = vw::vws;

namespace {

constexpr std::string_view usage =
    "usage: vw-shaderc [options] INPUT -o OUTPUT.vws\n"
    "       vw-shaderc --inspect FILE.vws\n"
    "\n"
    "INPUT is GLSL (compiled with glslc) or SPIR-V (.spv, wrapped as is).\n"
    "  -o FILE          output .vws file\n"
    "  -I DIR           add an include directory (vw's own is always searched)\n"
    "  -D NAME[=VALUE]  define a macro\n"
    "  --name NAME      shader name (default: from '// @vw name:' or the file name)\n"
    "  --depfile FILE   write a Makefile-style dependency file (for build systems)\n"
    "  --glslc PATH     the glslc to run (default: $VW_GLSLC, else glslc on PATH)\n"
    "  --inspect FILE   print a .vws file's metadata and exit\n";

std::vector<std::uint8_t> read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
  const fs::path temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write " + path.string());
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("cannot write " + path.string());
  }
  fs::rename(temporary, path);
}

std::string describe_flags(std::uint16_t flags) {
  std::string result;
  if ((flags & vws::reads_data) != 0) result += "data ";
  if ((flags & vws::reads_backdrop) != 0) result += "backdrop ";
  if ((flags & vws::uses_targets) != 0) result += "pass-images ";
  if ((flags & vws::animated) != 0) result += "time ";
  if (result.empty()) return "none";
  result.pop_back();  // the trailing space
  return result;
}

int inspect(const fs::path& path) {
  const auto shader = vws::decode(read_file(path));
  const auto interface = vws::inspect(shader.spirv);
  std::cout << "name:     " << shader.name << '\n'
            << "params:   ";
  for (std::size_t index = 0; index < shader.params.size(); ++index)
    std::cout << (index ? ", " : "") << "xyzw"[index] << " " << shader.params[index];
  if (shader.params.empty()) std::cout << "(unnamed)";
  std::cout << '\n'
            << "reads:    " << describe_flags(shader.flags) << '\n'
            << "bindings: ";
  for (std::size_t index = 0; index < interface.bindings.size(); ++index)
    std::cout << (index ? ", " : "") << interface.bindings[index];
  std::cout << '\n' << "spir-v:   " << shader.spirv.size() * 4 << " bytes\n";
  return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::vector<std::string> args(argv + 1, argv + argc);
    fs::path input, output, depfile;
    std::string name, glslc;
    std::vector<std::string> includes, defines;
    for (std::size_t index = 0; index < args.size(); ++index) {
      const std::string& arg = args[index];
      const auto value = [&]() -> std::string {
        if (index + 1 >= args.size()) throw std::runtime_error(arg + " needs a value");
        return args[++index];
      };
      if (arg == "-h" || arg == "--help") {
        std::cout << usage;
        return EXIT_SUCCESS;
      } else if (arg == "--inspect") {
        return inspect(value());
      } else if (arg == "-o") {
        output = value();
      } else if (arg == "-I") {
        includes.push_back(value());
      } else if (arg.starts_with("-I") && arg.size() > 2) {
        includes.push_back(arg.substr(2));
      } else if (arg == "-D") {
        defines.push_back(value());
      } else if (arg.starts_with("-D") && arg.size() > 2) {
        defines.push_back(arg.substr(2));
      } else if (arg == "--name") {
        name = value();
      } else if (arg == "--depfile") {
        depfile = value();
      } else if (arg == "--glslc") {
        glslc = value();
      } else if (arg.starts_with("-")) {
        throw std::runtime_error("unknown option " + arg + "\n" + std::string(usage));
      } else if (input.empty()) {
        input = arg;
      } else {
        throw std::runtime_error("only one input file");
      }
    }
    if (input.empty() || output.empty()) {
      std::cerr << usage;
      return EXIT_FAILURE;
    }

    vws::Shader shader;
    const auto bytes = read_file(input);
    if (vws::is_spirv(bytes)) {
      shader.name = name.empty() ? input.stem().string() : name;
      shader.spirv.resize(bytes.size() / 4);
      std::memcpy(shader.spirv.data(), bytes.data(), bytes.size());
      const std::string problem = vws::validate(vws::inspect(shader.spirv), shader.flags);
      if (!problem.empty()) throw std::runtime_error(input.string() + ": " + problem);
    } else {
      shader = vws::compile(input.string(), {.includes = includes, .defines = defines, .glslc = glslc,
                                             .name = name, .depfile = depfile.string(),
                                             .depfile_target = output.string()});
    }
    write_file(output, vws::encode(shader));
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "vw-shaderc: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
