#pragma once

#include <string_view>

namespace vw {

bool authenticate_password(std::string_view password, std::string_view service,
                           std::string_view pam_conf = {});

}  // namespace vw
