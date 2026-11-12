#include "vw/icon.hpp"

#include <gio/gio.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

namespace vw {

std::filesystem::path resolve_icon(std::string_view name, int size) {
  if (name.empty()) return {};
  const std::string text(name);
  if (g_path_is_absolute(text.c_str())) return text;
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  static std::unordered_map<std::string, std::filesystem::path> cache;
  const std::string key = text + ":" + std::to_string(std::clamp(size, 1, 1024));
  if (const auto found = cache.find(key); found != cache.end()) return found->second;
  static bool gtk_ready = gtk_init_check(nullptr, nullptr);
  if (!gtk_ready) return {};

  GError* error = nullptr;
  GIcon* icon = g_icon_new_for_string(text.c_str(), &error);
  if (error != nullptr) g_error_free(error);
  if (icon == nullptr) icon = g_themed_icon_new(text.c_str());
  if (G_IS_FILE_ICON(icon)) {
    GFile* file = g_file_icon_get_file(G_FILE_ICON(icon));
    char* path = g_file_get_path(file);
    const std::filesystem::path result = path == nullptr ? std::filesystem::path{} : path;
    g_free(path);
    g_object_unref(icon);
    cache[key] = result;
    return result;
  }

  GtkIconTheme* theme = gtk_icon_theme_get_default();
  GtkIconInfo* info = theme == nullptr ? nullptr : gtk_icon_theme_lookup_by_gicon(
      theme, icon, std::clamp(size, 1, 1024), GTK_ICON_LOOKUP_FORCE_SIZE);
  const char* filename = info == nullptr ? nullptr : gtk_icon_info_get_filename(info);
  const std::filesystem::path result = filename == nullptr ? std::filesystem::path{} : filename;
  if (info != nullptr) g_object_unref(info);
  g_object_unref(icon);
  if (cache.size() >= 2048) cache.clear();
  cache[key] = result;
  return result;
}

}  // namespace vw
