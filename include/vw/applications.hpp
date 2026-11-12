#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Installed desktop applications (GIO desktop entries).
namespace vw::applications {

struct Application {
  std::string id;
  std::string name;
  std::string display_name;
  std::string generic_name;
  std::string description;
  std::string executable;
  std::string commandline;
  std::string filename;
  std::string startup_wm_class;
  // Serialized GIcon, and the themed 48px file it resolves to.
  std::string icon;
  std::string icon_path;
  bool terminal = false;
  std::vector<std::string> categories;
  std::vector<std::string> keywords;
};

// Visible applications matching query, best match first: exact name, name
// prefix, word prefix, name substring, then other fields and keywords. An
// empty query lists everything by display name. A limit of 0 is unlimited.
std::vector<Application> search(std::string_view query = {}, std::size_t limit = 0);
// Launches a desktop entry by id, e.g. "firefox.desktop". Throws on failure.
void launch(std::string_view id);

}  // namespace vw::applications
