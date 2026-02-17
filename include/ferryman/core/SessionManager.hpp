#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ferryman::core {

struct SessionSnapshot {
  std::string token;
  std::string session_id;
  std::string client_ip;
  int64_t created_at = 0;
  int64_t last_seen_at = 0;
  bool command_authorized = true;
  bool screen_authorized = true;
};

class SessionManager {
 public:
  std::string CreateSession(const std::string& client_ip);
  std::optional<SessionSnapshot> GetSession(const std::string& token);
  bool Touch(const std::string& token);
  bool SetCommandAuthorization(const std::string& token, bool allowed);
  bool SetScreenAuthorization(const std::string& token, bool allowed);
  std::vector<SessionSnapshot> ListActiveSessions();

 private:
  std::mutex mu_;
  std::unordered_map<std::string, SessionSnapshot> sessions_;
};

}  // namespace ferryman::core
