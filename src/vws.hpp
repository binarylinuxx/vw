#pragma once

// VWS: vw's compiled shader format. A .vws file is SPIR-V plus what vw needs
// to know about it before drawing, so the renderer can trust and optimise
// it without guessing:
//
//   offset  size  field
//   0       4     magic "VWS1"
//   4       2     format version (1)
//   6       2     flags (Flag below)
//   8       4     SPIR-V length in 32-bit words
//   12      4     CRC-32 of the SPIR-V bytes
//   16      1     name length n, then n bytes of UTF-8
//           1     parameter count p (at most 4), then p times: 1 length + bytes
//           0-3   zero padding to a 4-byte boundary
//           4w    SPIR-V words
//
// Everything is little-endian. vw-shaderc writes these files; the renderer
// reads them, and still accepts plain SPIR-V for compatibility.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vw::vws {

inline constexpr std::uint16_t format_version = 1;

enum Flag : std::uint16_t {
  // Samples binding 2, the snapshot of what was drawn before it. Without
  // this flag the renderer skips the full-surface copy for the shader.
  reads_backdrop = 1U << 0U,
  // Reads binding 1, the shader_data storage buffer.
  reads_data = 1U << 1U,
  // Reads vw_time() (`#define VW_USE_TIME`): the surface redraws every frame
  // while the shader is on screen.
  animated = 1U << 2U,
  // Declares bindings 3 and 4, the named images of shader passes
  // (`#define VW_USE_TARGETS`).
  uses_targets = 1U << 3U,
};

struct Shader {
  std::string name;
  // What shader_params x, y, z, w mean; for tools and error messages.
  std::vector<std::string> params;
  std::uint16_t flags = 0;
  std::vector<std::uint32_t> spirv;
};

std::uint32_t crc32(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> encode(const Shader& shader);
// Throws std::runtime_error naming what is wrong with the file.
Shader decode(std::span<const std::uint8_t> bytes);
bool is_vws(std::span<const std::uint8_t> bytes);
bool is_spirv(std::span<const std::uint8_t> bytes);

// What a SPIR-V compute module declares, as far as vw cares.
struct Interface {
  bool has_main = false;          // a GLCompute entry point named "main"
  std::uint32_t local_size[3]{};  // from its LocalSize execution mode
  std::vector<std::uint32_t> bindings;  // descriptor bindings in set 0
  std::vector<std::uint32_t> other_sets;  // any descriptor set but 0
};
Interface inspect(std::span<const std::uint32_t> spirv);
// Empty when the module fits vw's rectangle shader contract; otherwise why
// not. Also derives the flags from the bindings it declares.
std::string validate(const Interface& interface, std::uint16_t& flags);

// GLSL to a validated .vws shader, through glslc. Metadata comes from
// "// @vw name:" and "// @vw params:" comments; `#define VW_USE_TIME` sets
// the animated flag. glslc's errors go to stderr; this throws when it fails.
struct CompileOptions {
  std::vector<std::string> includes;
  std::vector<std::string> defines;
  // Empty: $VW_GLSLC, else glslc on PATH.
  std::string glslc;
  // Overrides the name from the source or the file name.
  std::string name;
  // A Makefile-style dependency file naming `depfile_target`.
  std::string depfile;
  std::string depfile_target;
};
Shader compile(const std::string& source_path, const CompileOptions& options = {});

// Whether a file holds GLSL source rather than a .vws or SPIR-V binary.
bool is_glsl(std::span<const std::uint8_t> bytes);
// The flags of the shader at `path` (a .vws, SPIR-V or GLSL source), without
// compiling it; cached by modification time. 0 when it cannot be read.
std::uint16_t peek_flags(const std::string& path);

}  // namespace vw::vws
