#pragma once

#include <string>
#include <vector>

// Plain C++ that inbox.facet includes.
struct Message {
  std::string from;
  std::string subject;
  bool unread = true;
};

inline std::string initials(const std::string& name) {
  return name.empty() ? "?" : std::string(1, name.front());
}
