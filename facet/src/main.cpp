// facetc: compiles a .facet file into a C++ header for vw.
#include "syntax.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view usage =
    "usage: facetc INPUT.facet [-o OUTPUT.hpp] [--namespace NAME] [--module PATH]\n"
    "\n"
    "Writes C++ for each view in INPUT: a function template taking vw::Ui& and\n"
    "the view's parameters. Without -o the header goes to standard output.\n"
    "--module PATH takes elements written without a module from PATH, e.g.\n"
    "--module JES.spectralium for Surface(…) instead of S.Surface(…).\n";

std::string read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot read " + path);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

// file:line:column: error: message, then the line and a caret under it.
void report(const std::string& path, const std::string& source, const facet::Error& error) {
  std::cerr << path << ":" << error.at.line << ":" << error.at.column << ": error: " << error.what() << "\n";
  std::istringstream lines(source);
  std::string text;
  for (int line = 1; std::getline(lines, text); ++line) {
    if (line != error.at.line) continue;
    std::cerr << "  " << text << "\n  " << std::string(static_cast<std::size_t>(error.at.column - 1), ' ') << "^\n";
    break;
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string input;
  std::string output;
  std::string namespace_name = "views";
  std::string default_module;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if ((argument == "-o" || argument == "--namespace" || argument == "--module") && index + 1 < argc) {
      (argument == "-o" ? output : argument == "--module" ? default_module : namespace_name) = argv[++index];
    } else if (argument == "-h" || argument == "--help") {
      std::cout << usage;
      return EXIT_SUCCESS;
    } else if (input.empty() && !argument.starts_with('-')) {
      input = argument;
    } else {
      std::cerr << usage;
      return EXIT_FAILURE;
    }
  }
  if (input.empty()) {
    std::cerr << usage;
    return EXIT_FAILURE;
  }
  std::string source;
  try {
    source = read(input);
    const std::string header = facet::generate(facet::parse(source), input, namespace_name, default_module);
    if (output.empty()) {
      std::cout << header;
    } else {
      std::ofstream file(output, std::ios::binary);
      file << header;
      if (!file) throw std::runtime_error("cannot write " + output);
    }
  } catch (const facet::Error& error) {
    report(input, source, error);
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "facetc: " << error.what() << "\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
