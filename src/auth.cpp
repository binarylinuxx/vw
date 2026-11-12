#include "vw/auth.hpp"

#include <security/pam_appl.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <pwd.h>
#include <string>
#include <unistd.h>

namespace vw {
namespace {

int conversation(int count, const pam_message** messages,
                 pam_response** responses, void* user_data) {
  if (count < 1 || messages == nullptr || responses == nullptr || user_data == nullptr)
    return PAM_CONV_ERR;
  auto* results = static_cast<pam_response*>(std::calloc(
      static_cast<std::size_t>(count), sizeof(pam_response)));
  if (results == nullptr) return PAM_BUF_ERR;
  for (int index = 0; index < count; ++index) {
    switch (messages[index]->msg_style) {
      case PAM_PROMPT_ECHO_OFF:
        results[index].resp = ::strdup(static_cast<const char*>(user_data));
        if (results[index].resp != nullptr) continue;
        break;
      case PAM_TEXT_INFO:
      case PAM_ERROR_MSG:
        continue;
      default:
        break;
    }
    for (int item = 0; item <= index; ++item) {
      if (results[item].resp != nullptr) {
        explicit_bzero(results[item].resp, std::strlen(results[item].resp));
        std::free(results[item].resp);
      }
    }
    std::free(results);
    return PAM_CONV_ERR;
  }
  *responses = results;
  return PAM_SUCCESS;
}

}  // namespace

bool authenticate_password(std::string_view password, std::string_view service,
                           std::string_view pam_conf) {
  if (password.empty() || service.empty() || password.find('\0') != std::string_view::npos)
    return false;
  const passwd* user = getpwuid(getuid());
  if (user == nullptr || user->pw_name == nullptr) return false;
  std::string secret(password);
  std::string service_name(service);
  const pam_conv callbacks{conversation, secret.data()};
  pam_handle_t* handle = nullptr;
  int result = PAM_SERVICE_ERR;
  if (pam_conf.empty()) {
    result = pam_start(service_name.c_str(), user->pw_name, &callbacks, &handle);
  } else {
    const std::filesystem::path config(pam_conf);
    std::error_code error;
    if (config.is_absolute() && std::filesystem::is_regular_file(config, error)) {
      service_name = config.filename().string();
      const std::string directory = config.parent_path().string();
      result = pam_start_confdir(service_name.c_str(), user->pw_name,
                                 &callbacks, directory.c_str(), &handle);
    }
  }
  if (result == PAM_SUCCESS) result = pam_authenticate(handle, PAM_SILENT);
  if (handle != nullptr) pam_end(handle, result);
  explicit_bzero(secret.data(), secret.size());
  return result == PAM_SUCCESS;
}

}  // namespace vw
