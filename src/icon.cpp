// Icon names to files: serialized GIcon strings, file paths and URIs, and
// freedesktop icon themes (theme, its parents, hicolor, then unthemed).
#include "vw/icon.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace vw {
namespace {

namespace fs = std::filesystem;

std::vector<std::string> split(std::string_view text, char separator) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = text.find(separator, start);
    if (end == std::string_view::npos) end = text.size();
    std::string_view part = text.substr(start, end - start);
    while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.remove_prefix(1);
    while (!part.empty() && (part.back() == ' ' || part.back() == '\t' || part.back() == '\r')) part.remove_suffix(1);
    if (!part.empty()) parts.emplace_back(part);
    start = end + 1;
  }
  return parts;
}

std::string percent_decode(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) != 0 &&
        std::isxdigit(static_cast<unsigned char>(text[i + 2])) != 0) {
      out.push_back(static_cast<char>(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16)));
      i += 2;
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

std::string environment(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

// INI-style files: index.theme and GTK's settings.ini.
using Groups = std::map<std::string, std::map<std::string, std::string>, std::less<>>;

Groups read_ini(const fs::path& path) {
  Groups groups;
  std::ifstream input(path);
  std::string line, group;
  while (std::getline(input, line)) {
    std::string_view text(line);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) text.remove_suffix(1);
    if (text.empty() || text.front() == '#' || text.front() == ';') continue;
    if (text.front() == '[' && text.back() == ']') {
      group = std::string(text.substr(1, text.size() - 2));
      continue;
    }
    const auto equals = text.find('=');
    if (equals == std::string_view::npos) continue;
    std::string_view key = text.substr(0, equals), value = text.substr(equals + 1);
    while (!key.empty() && key.back() == ' ') key.remove_suffix(1);
    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    groups[group].emplace(std::string(key), std::string(value));
  }
  return groups;
}

// Where icons live: ~/.icons, $XDG_DATA_HOME/icons, $XDG_DATA_DIRS/icons,
// then /usr/share/pixmaps.
std::vector<fs::path> base_directories() {
  std::vector<fs::path> bases;
  const std::string home = environment("HOME");
  if (!home.empty()) bases.push_back(fs::path(home) / ".icons");
  const std::string data_home = environment("XDG_DATA_HOME");
  if (!data_home.empty() && data_home.front() == '/') bases.push_back(fs::path(data_home) / "icons");
  else if (!home.empty()) bases.push_back(fs::path(home) / ".local/share/icons");
  const std::string data_dirs = environment("XDG_DATA_DIRS");
  for (const std::string& directory : split(data_dirs.empty() ? "/usr/local/share:/usr/share" : data_dirs, ':'))
    if (directory.front() == '/') bases.push_back(fs::path(directory) / "icons");
  bases.emplace_back("/usr/share/pixmaps");
  return bases;
}

// The icon theme: $VWICONTHEME (or $VW_ICON_THEME), then gtk-icon-theme-name
// in GTK 4 and 3 settings.ini (user, then system), then Adwaita.
std::string theme_name() {
  for (const char* variable : {"VWICONTHEME", "VW_ICON_THEME"})
    if (std::string forced = environment(variable); !forced.empty()) return forced;
  std::vector<fs::path> configs;
  const std::string config_home = environment("XDG_CONFIG_HOME");
  const std::string home = environment("HOME");
  if (!config_home.empty() && config_home.front() == '/') configs.emplace_back(config_home);
  else if (!home.empty()) configs.push_back(fs::path(home) / ".config");
  const std::string config_dirs = environment("XDG_CONFIG_DIRS");
  for (const std::string& directory : split(config_dirs.empty() ? "/etc/xdg" : config_dirs, ':')) configs.emplace_back(directory);
  for (const fs::path& config : configs)
    for (const char* version : {"gtk-4.0", "gtk-3.0"}) {
      const Groups groups = read_ini(config / version / "settings.ini");
      if (const auto settings = groups.find("Settings"); settings != groups.end())
        if (const auto name = settings->second.find("gtk-icon-theme-name"); name != settings->second.end() && !name->second.empty())
          return name->second;
    }
  return "Adwaita";
}

struct Directory {
  std::string name;
  enum class Type { fixed, scalable, threshold } type = Type::threshold;
  int size = 0, scale = 1, min_size = 0, max_size = 0, threshold = 2;

  bool matches(int wanted) const {
    switch (type) {
      case Type::fixed: return size == wanted;
      case Type::scalable: return min_size <= wanted && wanted <= max_size;
      default: return size - threshold <= wanted && wanted <= size + threshold;
    }
  }
  int distance(int wanted) const {
    switch (type) {
      case Type::fixed: return std::abs(size - wanted);
      case Type::scalable: return wanted < min_size ? min_size - wanted : wanted > max_size ? wanted - max_size : 0;
      default:
        if (wanted < size - threshold) return min_size - wanted;
        if (wanted > size + threshold) return wanted - max_size;
        return 0;
    }
  }
};

struct Theme {
  std::string name;
  std::vector<fs::path> roots;  // <base>/<name> for every base that has it
  std::vector<Directory> directories;
  std::vector<std::string> parents;
};

int to_int(const std::map<std::string, std::string>& group, const char* key, int fallback) {
  const auto found = group.find(key);
  if (found == group.end()) return fallback;
  try {
    return std::stoi(found->second);
  } catch (const std::exception&) {
    return fallback;
  }
}

std::optional<Theme> load_theme(const std::string& name, const std::vector<fs::path>& bases) {
  Theme theme;
  theme.name = name;
  fs::path index;
  for (const fs::path& base : bases) {
    const fs::path root = base / name;
    std::error_code error;
    if (!fs::is_directory(root, error)) continue;
    theme.roots.push_back(root);
    if (index.empty() && fs::exists(root / "index.theme", error)) index = root / "index.theme";
  }
  if (index.empty()) return std::nullopt;
  const Groups groups = read_ini(index);
  const auto main = groups.find("Icon Theme");
  if (main == groups.end()) return std::nullopt;
  const auto get = [&](const char* key) {
    const auto found = main->second.find(key);
    return found == main->second.end() ? std::string() : found->second;
  };
  std::vector<std::string> names = split(get("Directories"), ',');
  for (const std::string& scaled : split(get("ScaledDirectories"), ','))
    if (std::ranges::find(names, scaled) == names.end()) names.push_back(scaled);
  for (const std::string& directory_name : names) {
    const auto group = groups.find(directory_name);
    if (group == groups.end()) continue;
    Directory directory;
    directory.name = directory_name;
    directory.size = to_int(group->second, "Size", 0);
    if (directory.size <= 0) continue;
    directory.scale = to_int(group->second, "Scale", 1);
    const auto type = group->second.find("Type");
    const std::string kind = type == group->second.end() ? "Threshold" : type->second;
    directory.type = kind == "Fixed" ? Directory::Type::fixed : kind == "Scalable" ? Directory::Type::scalable
                                                                                   : Directory::Type::threshold;
    directory.min_size = to_int(group->second, "MinSize", directory.size);
    directory.max_size = to_int(group->second, "MaxSize", directory.size);
    directory.threshold = to_int(group->second, "Threshold", 2);
    theme.directories.push_back(directory);
  }
  theme.parents = split(get("Inherits"), ',');
  return theme;
}

class IconThemes {
 public:
  fs::path lookup(const std::vector<std::string>& names, int size) {
    if (!loaded_) load();
    // GTK's order: every name in a theme before the next theme.
    for (const Theme& theme : chain_)
      for (const std::string& name : names)
        if (auto found = lookup_in(theme, name, size)) return *found;
    for (const std::string& name : names)
      for (const fs::path& base : bases_)
        for (const char* extension : {".png", ".svg"})
          if (exists(base, name + extension)) return base / (name + extension);
    return {};
  }

 private:
  void load() {
    loaded_ = true;
    bases_ = base_directories();
    std::set<std::string> seen;
    std::vector<std::string> pending{theme_name()};
    // Depth-first through Inherits, hicolor last.
    std::function<void(const std::string&)> add = [&](const std::string& name) {
      if (name == "hicolor" || !seen.insert(name).second) return;
      auto theme = load_theme(name, bases_);
      if (!theme) return;
      const std::vector<std::string> parents = theme->parents;
      chain_.push_back(std::move(*theme));
      for (const std::string& parent : parents) add(parent);
    };
    add(pending.front());
    if (auto hicolor = load_theme("hicolor", bases_)) chain_.push_back(std::move(*hicolor));
  }

  // Whether directory holds file, from a cached listing.
  bool exists(const fs::path& directory, const std::string& file) {
    auto [entry, inserted] = listings_.try_emplace(directory.string());
    if (inserted) {
      std::error_code error;
      for (auto it = fs::directory_iterator(directory, error); !error && it != fs::directory_iterator(); it.increment(error))
        entry->second.insert(it->path().filename().string());
    }
    return entry->second.contains(file);
  }

  std::optional<fs::path> lookup_in(const Theme& theme, const std::string& name, int size) {
    for (const Directory& directory : theme.directories) {
      if (directory.scale != 1 || !directory.matches(size)) continue;
      for (const fs::path& root : theme.roots)
        for (const char* extension : {".png", ".svg"})
          if (exists(root / directory.name, name + extension)) return root / directory.name / (name + extension);
    }
    std::optional<fs::path> closest;
    int best = std::numeric_limits<int>::max();
    for (const Directory& directory : theme.directories) {
      const int distance = directory.distance(size) + (directory.scale != 1 ? 1000 : 0);
      if (distance >= best) continue;
      for (const fs::path& root : theme.roots)
        for (const char* extension : {".png", ".svg"})
          if (exists(root / directory.name, name + extension)) {
            closest = root / directory.name / (name + extension);
            best = distance;
          }
    }
    return closest;
  }

  bool loaded_ = false;
  std::vector<fs::path> bases_;
  std::vector<Theme> chain_;
  std::unordered_map<std::string, std::unordered_set<std::string>> listings_;
};

// Names to try for one icon name: symbolic icons fall back to the regular
// one and regular icons to the symbolic one, as GIO's themed icons do.
std::vector<std::string> with_fallbacks(std::vector<std::string> names) {
  std::vector<std::string> extra;
  for (const std::string& name : names) {
    constexpr std::string_view symbolic = "-symbolic";
    const std::string other = name.ends_with(symbolic) ? name.substr(0, name.size() - symbolic.size())
                                                       : name + std::string(symbolic);
    if (std::ranges::find(names, other) == names.end() && std::ranges::find(extra, other) == extra.end())
      extra.push_back(other);
  }
  names.insert(names.end(), extra.begin(), extra.end());
  return names;
}

// A path for file icons, or theme names: ". GThemedIcon a b c" (serialized
// GIcon), "file:///...", "/path", or a bare name.
std::variant<fs::path, std::vector<std::string>> parse_icon(const std::string& text) {
  if (text.starts_with('/')) return fs::path(text);
  if (text.starts_with("file://")) {
    std::string_view rest = std::string_view(text).substr(7);
    if (!rest.starts_with("/")) {
      const auto slash = rest.find('/');
      rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
    }
    return fs::path(percent_decode(rest));
  }
  constexpr std::string_view themed = ". GThemedIcon ";
  if (text.starts_with(themed)) {
    std::vector<std::string> names;
    for (const std::string& token : split(std::string_view(text).substr(themed.size()), ' '))
      names.push_back(percent_decode(token));
    return names;
  }
  return std::vector<std::string>{text};
}

}  // namespace

std::filesystem::path resolve_icon(std::string_view name, int size) {
  if (name.empty()) return {};
  const std::string text(name);
  size = std::clamp(size, 1, 1024);
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  static std::unordered_map<std::string, std::filesystem::path> cache;
  const std::string key = text + ":" + std::to_string(size);
  if (const auto found = cache.find(key); found != cache.end()) return found->second;

  const auto parsed = parse_icon(text);
  std::filesystem::path result;
  if (const auto* path = std::get_if<fs::path>(&parsed)) {
    result = *path;
  } else {
    static IconThemes themes;
    result = themes.lookup(with_fallbacks(std::get<std::vector<std::string>>(parsed)), size);
  }
  if (cache.size() >= 2048) cache.clear();
  cache[key] = result;
  return result;
}

}  // namespace vw
