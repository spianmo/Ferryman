#include "ferryman/core/SessionManager.hpp"

#include "ferryman/util/Random.hpp"
#include "ferryman/util/Time.hpp"

namespace ferryman::core {

std::string SessionManager::CreateSession(const std::string& client_ip) {
  SessionSnapshot session;
  session.token = util::RandomHex(64);
  session.session_id = util::RandomHex(16);
  session.client_ip = client_ip;
  session.created_at = util::UtcNowEpochSeconds();
  session.last_seen_at = session.created_at;
  session.command_authorized = true;
  session.screen_authorized = true;

  std::lock_guard<std::mutex> lock(mu_);
  sessions_[session.token] = session;
  return session.token;
}

std::optional<SessionSnapshot> SessionManager::GetSession(const std::string& token) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(token);
  if (it == sessions_.end()) {
    return std::nullopt;
  }
  return it->second;
}

bool SessionManager::Touch(const std::string& token) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(token);
  if (it == sessions_.end()) {
    return false;
  }
  it->second.last_seen_at = util::UtcNowEpochSeconds();
  return true;
}

bool SessionManager::SetCommandAuthorization(const std::string& token, bool allowed) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(token);
  if (it == sessions_.end()) {
    return false;
  }
  it->second.command_authorized = allowed;
  it->second.last_seen_at = util::UtcNowEpochSeconds();
  return true;
}

bool SessionManager::SetScreenAuthorization(const std::string& token, bool allowed) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(token);
  if (it == sessions_.end()) {
    return false;
  }
  it->second.screen_authorized = allowed;
  it->second.last_seen_at = util::UtcNowEpochSeconds();
  return true;
}

std::vector<SessionSnapshot> SessionManager::ListActiveSessions() {
  std::vector<SessionSnapshot> snapshots;
  std::lock_guard<std::mutex> lock(mu_);
  snapshots.reserve(sessions_.size());
  for (const auto& [_, session] : sessions_) {
    snapshots.push_back(session);
  }
  return snapshots;
}

}  // namespace ferryman::core
