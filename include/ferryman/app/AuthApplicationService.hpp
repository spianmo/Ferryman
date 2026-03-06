#pragma once

#include "ferryman/core/AuditLogger.hpp"
#include "ferryman/core/SessionManager.hpp"
#include "ferryman/util/StringUtil.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace ferryman::app {

class AuthApplicationService {
 public:
  AuthApplicationService(core::SessionManager& session_manager, core::AuditLogger& audit_logger)
      : session_manager_(session_manager), audit_logger_(audit_logger) {}

  bool ValidateAccessKey(const std::string& provided, const std::string& expected) const {
    const std::string normalized_expected = NormalizeAccessKey(expected);
    if (normalized_expected.empty()) {
      return false;
    }
    return NormalizeAccessKey(provided) == normalized_expected;
  }

  std::string CreateSession(const std::string& client_ip) {
    const std::string token = session_manager_.CreateSession(client_ip);
    audit_logger_.Append(token, "auth.login", "login succeeded");
    return token;
  }

  std::optional<core::SessionSnapshot> GetSession(const std::string& token) {
    return session_manager_.GetSession(token);
  }

  bool TouchSession(const std::string& token) {
    return session_manager_.Touch(token);
  }

 private:
  static std::string NormalizeAccessKey(std::string value) {
    if (value.size() >= 3 && static_cast<unsigned char>(value[0]) == 0xEF &&
        static_cast<unsigned char>(value[1]) == 0xBB && static_cast<unsigned char>(value[2]) == 0xBF) {
      value.erase(0, 3);
    }

    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
                  return c == '\0';
                }),
                value.end());

    value = util::Trim(value);
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
      value = util::Trim(value.substr(1, value.size() - 2));
    }

    auto is_control = [](unsigned char c) {
      return (c <= 0x1f || c == 0x7f) && !std::isspace(c);
    };
    value.erase(std::remove_if(value.begin(), value.end(), is_control), value.end());
    return util::Trim(value);
  }

  core::SessionManager& session_manager_;
  core::AuditLogger& audit_logger_;
};

}  // namespace ferryman::app
