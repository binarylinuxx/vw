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

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r");
  return value.substr(first, last - first + 1);
}

// "// @vw name: X" and "// @vw params: a, b, c" lines.
void read_metadata(const std::string& source, vws::Shader& shader) {
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
      if (shader.params.size() > 4)
        throw std::runtime_error("'@vw params' names " + std::to_string(shader.params.size()) +
                                 " parameters; shader_params holds 4");
    }
  }
}

int run(const std::vector<std::string>& argv) {
  std::vector<char*> arguments;
  for (const auto& argument : argv) arguments.push_back(const_cast<char*>(argument.c_str()));
  arguments.push_back(nullptr);
  const pid_t child = fork();
  if (child < 0) throw std::runtime_error("cannot start " + argv.front());
  if (child == 0) {
    execvp(arguments.front(), arguments.data());
    std::fprintf(stderr, "vw-shaderc: cannot run %s\n", arguments.front());
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

std::string describe_flags(std::uint16_t flags) {
  std::string result;
  if ((flags & vws::reads_data) != 0) result += "data ";
  if ((flags & vws::reads_backdrop) != 0) result += "backdrop ";
  return result.empty() ? "none" : trim(result);
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
    shader.name = input.stem().string();
    const auto bytes = read_file(input);
    std::vector<std::uint8_t> code;
    if (vws::is_spirv(bytes)) {
      code = bytes;
    } else {
      read_metadata(std::string(bytes.begin(), bytes.end()), shader);
      if (glslc.empty()) {
        const char* environment = std::getenv("VW_GLSLC");
        glslc = environment != nullptr && *environment != '\0' ? environment : "glslc";
      }
      const fs::path compiled = output.string() + ".spv.tmp";
      std::vector<std::string> command{glslc, "-fshader-stage=compute", "--target-env=vulkan1.0", "-O"};
      for (const auto& directory : includes) command.push_back("-I" + directory);
#ifdef VW_SHADER_INCLUDE_DIRS
      // vw's own headers, so `#include <vw/shader.glsl>` always resolves.
      std::istringstream builtin(VW_SHADER_INCLUDE_DIRS);
      for (std::string directory; std::getline(builtin, directory, ':');)
        if (!directory.empty()) command.push_back("-I" + directory);
#endif
      for (const auto& define : defines) command.push_back("-D" + define);
      if (!depfile.empty()) {
        command.insert(command.end(), {"-MD", "-MF", depfile.string(), "-MT", output.string()});
      }
      command.insert(command.end(), {input.string(), "-o", compiled.string()});
      if (run(command) != 0) {
        std::error_code ignored;
        fs::remove(compiled, ignored);
        std::cerr << "vw-shaderc: glslc failed for " << input.string() << '\n';
        return EXIT_FAILURE;
      }
      code = read_file(compiled);
      fs::remove(compiled);
      if (!vws::is_spirv(code)) throw std::runtime_error("glslc did not produce SPIR-V");
    }
    if (!name.empty()) shader.name = name;
    shader.spirv.resize(code.size() / 4);
    std::memcpy(shader.spirv.data(), code.data(), code.size());

    const std::string problem = vws::validate(vws::inspect(shader.spirv), shader.flags);
    if (!problem.empty()) throw std::runtime_error(input.string() + ": " + problem);
    write_file(output, vws::encode(shader));
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "vw-shaderc: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
