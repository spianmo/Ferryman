#include "ferryman/codeagent/CodeAgentManager.hpp"

#include "ferryman/util/Random.hpp"
#include "ferryman/util/StringUtil.hpp"

#include <algorithm>
#include <chrono>
#include <system_error>

namespace ferryman::codeagent {

nlohmann::json CodeAgentManager::BuildSessionsResponse(const std::string& ns) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<const SessionRecord*> sessions;
  auto ns_it = session_ids_by_ns_.find(ns);
  if (ns_it != session_ids_by_ns_.end()) {
    sessions.reserve(ns_it->second.size());
    for (const auto& id : ns_it->second) {
      auto it = sessions_by_id_.find(id);
      if (it != sessions_by_id_.end()) {
        sessions.push_back(&it->second);
      }
    }
  }

  std::sort(sessions.begin(), sessions.end(), [](const SessionRecord* lhs, const SessionRecord* rhs) {
    if (lhs->active != rhs->active) {
      return lhs->active;
    }
    return lhs->updated_at_ms > rhs->updated_at_ms;
  });

  nlohmann::json serialized = nlohmann::json::array();
  for (const auto* session : sessions) {
    serialized.push_back(BuildSessionSummaryJsonLocked(*session));
  }
  return {{"sessions", serialized}};
}

std::optional<nlohmann::json> CodeAgentManager::BuildSessionResponse(const std::string& ns,
                                                                     const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto session = GetSessionLocked(ns, session_id);
  if (!session.has_value()) {
    return std::nullopt;
  }
  return nlohmann::json{{"session", BuildSessionJsonLocked(**session)}};
}

std::optional<nlohmann::json> CodeAgentManager::BuildMessagesResponse(const std::string& ns,
                                                                      const std::string& session_id, int limit,
                                                                      std::optional<int> before_seq) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto session_opt = GetSessionLocked(ns, session_id);
  if (!session_opt.has_value()) {
    return std::nullopt;
  }

  const SessionRecord& session = **session_opt;
  std::vector<const MessageRecord*> filtered;
  filtered.reserve(session.messages.size());

  for (const auto& message : session.messages) {
    if (before_seq.has_value() && message.seq >= *before_seq) {
      continue;
    }
    filtered.push_back(&message);
  }

  const int bounded_limit = std::clamp(limit, 1, 200);
  bool has_more = static_cast<int>(filtered.size()) > bounded_limit;
  if (has_more) {
    filtered.erase(filtered.begin(), filtered.end() - bounded_limit);
  }

  nlohmann::json messages = nlohmann::json::array();
  for (const auto* message : filtered) {
    messages.push_back(BuildMessageJson(*message));
  }

  nlohmann::json next_before_seq = nullptr;
  if (has_more && !filtered.empty()) {
    next_before_seq = filtered.front()->seq;
  }

  nlohmann::json before_value = nullptr;
  if (before_seq.has_value()) {
    before_value = *before_seq;
  }

  return nlohmann::json{
      {"messages", messages},
      {"page", {
                   {"limit", bounded_limit},
                   {"beforeSeq", before_value},
                   {"nextBeforeSeq", next_before_seq},
                   {"hasMore", has_more},
               }},
  };
}

nlohmann::json CodeAgentManager::BuildMachinesResponse(const std::string& ns) const {
  (void)ns;
  std::lock_guard<std::mutex> lock(mu_);
  return {
      {"machines", nlohmann::json::array({
                       {
                           {"id", machine_.id},
                           {"active", machine_.active},
                           {"metadata", machine_.metadata},
                       },
                   })},
  };
}

std::optional<std::filesystem::path> CodeAgentManager::ResolveSessionPath(const std::string& ns,
                                                                          const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto session = GetSessionLocked(ns, session_id);
  if (!session.has_value()) {
    return std::nullopt;
  }
  return (**session).path;
}

std::optional<std::string> CodeAgentManager::ResolveSessionFlavor(const std::string& ns,
                                                                  const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto session = GetSessionLocked(ns, session_id);
  if (!session.has_value()) {
    return std::nullopt;
  }
  return (**session).flavor;
}

nlohmann::json CodeAgentManager::BuildSlashCommandsResponse(const std::string& ns,
                                                            const std::string& session_id) const {
  const auto flavor = ResolveSessionFlavor(ns, session_id);
  if (!flavor.has_value()) {
    return {
        {"success", false},
        {"error", "session not found"},
    };
  }

  nlohmann::json commands = nlohmann::json::array();
  const auto push_command = [&commands](const std::string& name, const std::string& description) {
    commands.push_back({
        {"name", name},
        {"description", description},
        {"source", "builtin"},
    });
  };

  push_command("/help", "Show built-in commands");
  push_command("/status", "Show current session state");
  push_command("/clear", "Clear visible conversation");
  if (*flavor == "codex") {
    push_command("/review", "Review pending changes");
    push_command("/permissions", "Change permission mode");
  } else if (*flavor == "cursor") {
    push_command("/plan", "Switch to planning mode");
    push_command("/run", "Execute approved plan");
  } else if (*flavor == "gemini") {
    push_command("/explain", "Explain selected code or output");
    push_command("/fix", "Request targeted code fixes");
  } else if (*flavor == "opencode") {
    push_command("/commit", "Summarize and prepare commit text");
  } else {
    push_command("/model", "Switch between available Claude models");
    push_command("/permissions", "Change permission mode");
  }

  return {
      {"success", true},
      {"commands", commands},
  };
}

nlohmann::json CodeAgentManager::BuildSkillsResponse(const std::string& ns, const std::string& session_id) const {
  if (!ResolveSessionPath(ns, session_id).has_value()) {
    return {
        {"success", false},
        {"error", "session not found"},
    };
  }

  return {
      {"success", true},
      {"skills", nlohmann::json::array({
                     {{"name", "terminal"}, {"description", "Execute shell commands in the session workspace"}},
                     {{"name", "patch"}, {"description", "Apply edits directly to project files"}},
                     {{"name", "search"}, {"description", "Search codebase symbols and references quickly"}},
                 })},
  };
}

bool CodeAgentManager::CheckPathsExist(const std::string& ns, const std::vector<std::string>& paths,
                                       nlohmann::json* exists, std::string* error) const {
  (void)ns;
  if (exists == nullptr) {
    if (error != nullptr) {
      *error = "invalid output target";
    }
    return false;
  }

  nlohmann::json result = nlohmann::json::object();
  for (const auto& raw : paths) {
    const std::string path_text = util::Trim(raw);
    if (path_text.empty()) {
      continue;
    }

    std::error_code ec;
    std::filesystem::path path(path_text);
    if (path.is_relative()) {
      path = std::filesystem::absolute(path, ec);
    }
    if (ec) {
      result[path_text] = false;
      continue;
    }
    result[path_text] = std::filesystem::exists(path, ec) && !ec;
  }

  *exists = std::move(result);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool CodeAgentManager::ResumeSession(const std::string& ns, const std::string& session_id, std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  auto session_opt = GetMutableSessionLocked(ns, session_id);
  if (!session_opt.has_value()) {
    if (error != nullptr) {
      *error = "session not found";
    }
    return false;
  }
  SessionRecord& session = **session_opt;
  session.active = true;
  session.active_at_ms = NowMs();
  session.updated_at_ms = session.active_at_ms;
  PushEventLocked(ns,
                  {
                      {"type", "session-updated"},
                      {"namespace", ns},
                      {"sessionId", session.id},
                      {"data", BuildSessionJsonLocked(session)},
                  },
                  session.id);
  PersistStateLocked();
  return true;
}

bool CodeAgentManager::AbortSession(const std::string& ns, const std::string& session_id, std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  auto session_opt = GetMutableSessionLocked(ns, session_id);
  if (!session_opt.has_value()) {
    if (error != nullptr) {
      *error = "session not found";
    }
    return false;
  }
  SessionRecord& session = **session_opt;
  session.generation += 1;
  session.thinking = false;
  session.updated_at_ms = NowMs();

  if (!session.agent_state_requests.is_object()) {
    session.agent_state_requests = nlohmann::json::object();
  }
  if (!session.agent_state_completed_requests.is_object()) {
    session.agent_state_completed_requests = nlohmann::json::object();
  }
  for (auto request_it = session.agent_state_requests.begin(); request_it != session.agent_state_requests.end();) {
    if (!request_it->is_object()) {
      request_it = session.agent_state_requests.erase(request_it);
      continue;
    }

    const nlohmann::json request = *request_it;
    const std::string request_id = request_it.key();
    const std::string request_tool = request.value("tool", std::string("Tool"));
    const nlohmann::json request_arguments = request.value("arguments", nlohmann::json::object());
    request_it = session.agent_state_requests.erase(request_it);
    session.agent_state_completed_requests[request_id] = {
        {"tool", request_tool},
        {"arguments", request_arguments},
        {"createdAt", request.value("createdAt", session.updated_at_ms)},
        {"completedAt", session.updated_at_ms},
        {"status", "denied"},
        {"decision", "abort"},
        {"reason", "Aborted by user"},
    };
  }

  if (!InterruptSessionRunnerLocked(session.id, error)) {
    return false;
  }
  PushEventLocked(ns,
                  {
                      {"type", "session-updated"},
                      {"namespace", ns},
                      {"sessionId", session.id},
                      {"data", BuildSessionJsonLocked(session)},
                  },
                  session.id);
  PersistStateLocked();
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool CodeAgentManager::ArchiveSession(const std::string& ns, const std::string& session_id, std::string* error) {
  std::shared_ptr<SessionRunnerState> runner;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto session_opt = GetMutableSessionLocked(ns, session_id);
    if (!session_opt.has_value()) {
      if (error != nullptr) {
        *error = "session not found";
      }
      return false;
    }
    SessionRecord& session = **session_opt;
    session.active = false;
    session.thinking = false;
    session.updated_at_ms = NowMs();
    auto runner_it = session_runners_.find(session.id);
    if (runner_it != session_runners_.end()) {
      runner = runner_it->second;
      session_runners_.erase(runner_it);
    }
    PushEventLocked(ns,
                    {
                        {"type", "session-updated"},
                        {"namespace", ns},
                        {"sessionId", session.id},
                        {"data", BuildSessionJsonLocked(session)},
                    },
                    session.id);
    PersistStateLocked();
  }

  StopSessionRunner(runner);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool CodeAgentManager::RenameSession(const std::string& ns, const std::string& session_id, const std::string& name,
                                     std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  auto session_opt = GetMutableSessionLocked(ns, session_id);
  if (!session_opt.has_value()) {
    if (error != nullptr) {
      *error = "session not found";
    }
    return false;
  }
  SessionRecord& session = **session_opt;
  const std::string normalized_name = util::Trim(name);
  const bool title_changed = normalized_name != session.name;
  session.name = normalized_name;
  if (title_changed && !normalized_name.empty()) {
    session.title_initialized = true;
  }
  session.updated_at_ms = NowMs();
  if (title_changed && !normalized_name.empty()) {
    EmitAgentEventMessageLocked(session,
                                {
                                    {"type", "title-changed"},
                                    {"title", normalized_name},
                                });
  }
  PushEventLocked(ns,
                  {
                      {"type", "session-updated"},
                      {"namespace", ns},
                      {"sessionId", session.id},
                      {"data", BuildSessionJsonLocked(session)},
                  },
                  session.id);
  PersistStateLocked();
  return true;
}

bool CodeAgentManager::DeleteSession(const std::string& ns, const std::string& session_id, std::string* error) {
  std::shared_ptr<SessionRunnerState> runner;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto session_it = sessions_by_id_.find(session_id);
    if (session_it == sessions_by_id_.end() || session_it->second.ns != ns) {
      if (error != nullptr) {
        *error = "session not found";
      }
      return false;
    }

    auto runner_it = session_runners_.find(session_id);
    if (runner_it != session_runners_.end()) {
      runner = runner_it->second;
      session_runners_.erase(runner_it);
    }

    sessions_by_id_.erase(session_it);
    auto list_it = session_ids_by_ns_.find(ns);
    if (list_it != session_ids_by_ns_.end()) {
      auto& ids = list_it->second;
      ids.erase(std::remove(ids.begin(), ids.end(), session_id), ids.end());
    }

    PushEventLocked(ns,
                    {
                        {"type", "session-removed"},
                        {"namespace", ns},
                        {"sessionId", session_id},
                    },
                    session_id);
    PersistStateLocked();
  }

  StopSessionRunner(runner);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

nlohmann::json CodeAgentManager::BuildRunnerState(const std::string& ns) const {
  std::lock_guard<std::mutex> lock(mu_);
  size_t session_count = 0;
  auto it = session_ids_by_ns_.find(ns);
  if (it != session_ids_by_ns_.end()) {
    session_count = it->second.size();
  }

  return {
      {"machine", {
                      {"id", machine_.id},
                      {"active", machine_.active},
                      {"metadata", machine_.metadata},
                  }},
      {"runner", {
                     {"status", "running"},
                     {"updatedAt", NowMs()},
                     {"trackedSessions", session_count},
                 }},
  };
}

nlohmann::json CodeAgentManager::DrainEventsForToken(const std::string& bearer_token, const std::string& ns,
                                                     const EventQuery& query, const std::string& visibility,
                                                     int wait_ms) {
  (void)visibility;
  std::unique_lock<std::mutex> lock(mu_);

  std::uint64_t cursor = 0;
  auto cursor_it = token_event_cursor_.find(bearer_token);
  if (cursor_it != token_event_cursor_.end()) {
    cursor = cursor_it->second;
  }

  const auto matches_query = [&query, &ns](const EventRecord& event) -> bool {
    if (event.ns != ns) {
      return false;
    }
    if (query.all) {
      return true;
    }
    if (!query.session_id.empty() && !event.session_id.empty() && event.session_id != query.session_id) {
      return false;
    }
    if (!query.machine_id.empty() && !event.machine_id.empty() && event.machine_id != query.machine_id) {
      return false;
    }
    return true;
  };

  if (wait_ms > 0) {
    const auto has_new_match = [this, &matches_query, &cursor]() -> bool {
      for (const auto& event : events_) {
        if (event.id > cursor && matches_query(event)) {
          return true;
        }
      }
      return false;
    };
    if (!has_new_match()) {
      event_cv_.wait_for(lock, std::chrono::milliseconds(wait_ms), has_new_match);
    }
  }

  nlohmann::json out_events = nlohmann::json::array();
  for (const auto& event : events_) {
    if (event.id <= cursor) {
      continue;
    }
    if (!matches_query(event)) {
      continue;
    }

    out_events.push_back(event.payload);
    cursor = event.id;
  }
  token_event_cursor_[bearer_token] = cursor;

  nlohmann::json response = {
      {"subscriptionId", "sub-" + util::RandomHex(12)},
      {"events", out_events},
      {"cursor", cursor},
  };
  return response;
}

}  // namespace ferryman::codeagent
