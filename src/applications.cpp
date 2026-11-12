#include "vw/applications.hpp"
#include "vw/icon.hpp"

#include <gio/gdesktopappinfo.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vw::applications {
namespace {

using DesktopInfo = std::unique_ptr<GDesktopAppInfo, decltype(&g_object_unref)>;

std::string string_or_empty(const char* value) {
  return value == nullptr ? std::string{} : std::string(value);
}

std::vector<std::string> split_categories(const char* categories) {
  std::vector<std::string> result;
  if (categories == nullptr) return result;
  std::string_view remaining(categories);
  while (!remaining.empty()) {
    const std::size_t separator = remaining.find(';');
    const std::string_view category = remaining.substr(0, separator);
    if (!category.empty()) result.emplace_back(category);
    if (separator == std::string_view::npos) break;
    remaining.remove_prefix(separator + 1);
  }
  return result;
}

Application describe(GDesktopAppInfo* desktop) {
  GAppInfo* app = G_APP_INFO(desktop);
  Application result;
  result.id = string_or_empty(g_app_info_get_id(app));
  result.name = string_or_empty(g_app_info_get_name(app));
  result.display_name = string_or_empty(g_app_info_get_display_name(app));
  result.generic_name = string_or_empty(g_desktop_app_info_get_generic_name(desktop));
  result.description = string_or_empty(g_app_info_get_description(app));
  result.executable = string_or_empty(g_app_info_get_executable(app));
  result.commandline = string_or_empty(g_app_info_get_commandline(app));
  result.filename = string_or_empty(g_desktop_app_info_get_filename(desktop));
  result.startup_wm_class = string_or_empty(g_desktop_app_info_get_startup_wm_class(desktop));
  if (GIcon* icon = g_app_info_get_icon(app)) {
    gchar* serialized = g_icon_to_string(icon);
    result.icon = string_or_empty(serialized);
    g_free(serialized);
  }
  result.icon_path = resolve_icon(result.icon, 48).string();
  result.terminal = g_desktop_app_info_get_boolean(desktop, "Terminal");
  result.categories = split_categories(g_desktop_app_info_get_categories(desktop));
  if (const char* const* keywords = g_desktop_app_info_get_keywords(desktop))
    for (; *keywords != nullptr; ++keywords) result.keywords.emplace_back(*keywords);
  return result;
}

std::vector<DesktopInfo> applications() {
  std::vector<DesktopInfo> result;
  GList* infos = g_app_info_get_all();
  for (GList* item = infos; item != nullptr; item = item->next) {
    GAppInfo* app = G_APP_INFO(item->data);
    if (!G_IS_DESKTOP_APP_INFO(app) || !g_app_info_should_show(app)) {
      g_object_unref(app);
      continue;
    }
    result.emplace_back(G_DESKTOP_APP_INFO(app), &g_object_unref);
  }
  g_list_free(infos);
  std::ranges::sort(result, {}, [](const DesktopInfo& app) {
    return string_or_empty(g_app_info_get_display_name(G_APP_INFO(app.get())));
  });
  return result;
}

std::string lowercase(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(),
      [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
  return result;
}

// Lower ranks sort first: exact name, name prefix, word prefix, name
// substring, then any other searchable field or keyword.
std::optional<int> match_rank(GDesktopAppInfo* desktop, std::string_view needle) {
  if (needle.empty()) return 0;
  GAppInfo* app = G_APP_INFO(desktop);
  int best = std::numeric_limits<int>::max();
  for (const char* value : {g_app_info_get_display_name(app), g_app_info_get_name(app)}) {
    if (value == nullptr) continue;
    const std::string name = lowercase(value);
    const std::size_t found = name.find(needle);
    if (found == std::string::npos) continue;
    if (name.size() == needle.size()) best = std::min(best, 0);
    else if (found == 0) best = std::min(best, 1);
    else if (!std::isalnum(static_cast<unsigned char>(name[found - 1]))) best = std::min(best, 2);
    else best = std::min(best, 3);
  }
  if (best != std::numeric_limits<int>::max()) return best;
  for (const char* value : {g_desktop_app_info_get_generic_name(desktop),
                            g_app_info_get_description(app), g_app_info_get_executable(app),
                            g_app_info_get_id(app)}) {
    if (value != nullptr && lowercase(value).contains(needle)) return 4;
  }
  if (const char* const* keywords = g_desktop_app_info_get_keywords(desktop)) {
    for (; *keywords != nullptr; ++keywords)
      if (lowercase(*keywords).contains(needle)) return 4;
  }
  return std::nullopt;
}

}  // namespace

std::vector<Application> search(std::string_view query, std::size_t limit) {
  const std::string needle = lowercase(query);
  const auto apps = applications();
  std::vector<std::pair<int, GDesktopAppInfo*>> matches;
  for (const DesktopInfo& app : apps)
    if (const auto rank = match_rank(app.get(), needle)) matches.emplace_back(*rank, app.get());
  // applications() is sorted by display name, so a stable sort keeps that order per rank.
  std::ranges::stable_sort(matches, {}, &std::pair<int, GDesktopAppInfo*>::first);
  if (limit > 0 && matches.size() > limit) matches.resize(limit);
  std::vector<Application> result;
  result.reserve(matches.size());
  for (const auto& [rank, app] : matches) result.push_back(describe(app));
  return result;
}

void launch(std::string_view id) {
  const std::string name(id);
  DesktopInfo app(g_desktop_app_info_new(name.c_str()), &g_object_unref);
  if (!app) throw std::runtime_error("desktop application '" + name + "' was not found");
  GError* error = nullptr;
  if (!g_app_info_launch(G_APP_INFO(app.get()), nullptr, nullptr, &error)) {
    const std::string message = error == nullptr ? "application launch failed" : error->message;
    if (error != nullptr) g_error_free(error);
    throw std::runtime_error(message);
  }
}

}  // namespace vw::applications
