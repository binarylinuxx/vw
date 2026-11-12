#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include "vw/app.hpp"
#include "vw/scene.hpp"

// Deprecated: the Lua configuration frontend. New code should use the C++ API
// (vw/app.hpp, vw/ui.hpp); this layer only translates Lua tables into it.
namespace vw {

// Runs a Lua configuration file once and returns the scene it built.
Scene load_config(const std::filesystem::path& path);

// Rebuilds from a Lua file, hot-reloading when it or a sibling .lua/.js
// file changes, and refreshing on Lua service events.
class LuaSource : public SceneSource {
 public:
  explicit LuaSource(std::filesystem::path path) : path_(std::move(path)) {}

  Scene build() override { return load_config(path_); }
  std::vector<EventSource> event_sources() override;
  std::optional<std::filesystem::path> watch_directory() const override;
  bool affects(std::string_view file_name) const override;

 private:
  std::filesystem::path path_;
};

}  // namespace vw
