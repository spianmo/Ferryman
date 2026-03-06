#include "ferryman/codeagent/CodeAgentManager.hpp"

#include "CodeAgentPolicy.hpp"
#include "ferryman/util/Random.hpp"
#include "ferryman/util/StringUtil.hpp"

#include <filesystem>
#include <system_error>

namespace ferryman::codeagent {

SpawnSessionResult CodeAgentManager::SpawnSession(const std::string& ns, const SpawnSessionOptions& options) {
  std::string directory = util::Trim(options.directory);
  if (directory.empty()) {
    return SpawnSessionResult{.type = "error", .message = "directory is required"};
  }

  std::error_code ec;
  std::filesystem::path path = std::filesystem::absolute(std::filesystem::path(directory), ec);
  if (ec) {
    return SpawnSessionResult{.type = "error", .message = "failed to resolve directory"};
  }

  std::filesystem::create_directories(path, ec);
  if (ec) {
    return SpawnSessionResult{.type = "error", .message = "failed to create directory: " + ec.message()};
  }

  SessionRecord session;
  session.id = "session-" + util::RandomHex(20);
  session.ns = ns;
  session.created_at_ms = NowMs();
  session.updated_at_ms = session.created_at_ms;
  session.active_at_ms = session.created_at_ms;
  session.thinking_at_ms = session.created_at_ms;
  session.path = path;
  session.host = "localhost";
  session.machine_id = machine_.id;
  session.flavor = NormalizeAgent(options.agent);
  session.name = path.filename().string();
  session.model = util::Trim(options.model);
  session.permission_mode = policy::ResolveSpawnPermissionMode(session.flavor, options.yolo);
  if (session.flavor == "codex") {
    session.model_reasoning_effort = "medium";
  }
  session.yolo = options.yolo;
  session.title_initialized = false;
  session.agent_state_requests = nlohmann::json::object();
  session.agent_state_completed_requests = nlohmann::json::object();

  {
    std::lock_guard<std::mutex> lock(mu_);
    sessions_by_id_[session.id] = session;
    session_ids_by_ns_[ns].push_back(session.id);

    PushEventLocked(ns,
                    {
                        {"type", "session-added"},
                        {"namespace", ns},
                        {"sessionId", session.id},
                        {"data", BuildSessionJsonLocked(session)},
                    },
                    session.id);
    PersistStateLocked();
  }

  return SpawnSessionResult{.type = "success", .session_id = session.id};
}

bool CodeAgentManager::SetPermissionMode(const std::string& ns, const std::string& session_id, const std::string& mode,
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
  const std::string normalized = policy::NormalizePermissionModeValue(mode);
  if (normalized.empty()) {
    if (error != nullptr) {
      *error = "invalid permission mode for session flavor";
    }
    return false;
  }
  if (!session.active) {
    if (error != nullptr) {
      *error = "session is inactive";
    }
    return false;
  }
  if (!policy::IsPermissionModeAllowedForFlavor(normalized, session.flavor)) {
    if (error != nullptr) {
      *error = "invalid permission mode for session flavor";
    }
    return false;
  }

  const bool mode_changed = session.permission_mode != normalized;
  session.permission_mode = normalized;
  if (session.flavor == "claude") {
    session.yolo = normalized == "bypassPermissions";
  } else if (policy::IsCodexOrGeminiFlavor(session.flavor) || session.flavor == "cursor" ||
             session.flavor == "opencode") {
    session.yolo = normalized == "yolo";
  }
  session.updated_at_ms = NowMs();
  if (mode_changed) {
    EmitAgentEventMessageLocked(session,
                                {
                                    {"type", "permission-mode-changed"},
                                    {"mode", normalized},
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

bool CodeAgentManager::SetModelMode(const std::string& ns, const std::string& session_id, const std::string& model,
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
  const std::string normalized = util::Trim(model);
  if (normalized.empty() || !policy::IsKnownModelMode(normalized)) {
    if (error != nullptr) {
      *error = "invalid model mode";
    }
    return false;
  }
  if (!session.active) {
    if (error != nullptr) {
      *error = "session is inactive";
    }
    return false;
  }
  if (!policy::IsModelModeAllowedForFlavor(normalized, session.flavor)) {
    if (error != nullptr) {
      *error = "model mode is only supported for claude sessions";
    }
    return false;
  }
  session.model_mode = normalized;
  session.updated_at_ms = NowMs();
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

bool CodeAgentManager::SetModelReasoningEffort(const std::string& ns, const std::string& session_id,
                                               const std::string& effort, std::string* error) {
  std::lock_guard<std::mutex> lock(mu_);
  auto session_opt = GetMutableSessionLocked(ns, session_id);
  if (!session_opt.has_value()) {
    if (error != nullptr) {
      *error = "session not found";
    }
    return false;
  }

  SessionRecord& session = **session_opt;
  const std::string normalized = policy::NormalizeReasoningEffortValue(effort);
  if (normalized.empty() || !policy::IsKnownReasoningEffort(normalized)) {
    if (error != nullptr) {
      *error = "invalid model reasoning effort";
    }
    return false;
  }
  if (!session.active) {
    if (error != nullptr) {
      *error = "session is inactive";
    }
    return false;
  }
  if (!policy::IsReasoningEffortAllowedForFlavor(normalized, session.flavor)) {
    if (error != nullptr) {
      *error = "model reasoning effort is only supported for codex sessions";
    }
    return false;
  }

  session.model_reasoning_effort = normalized;
  session.updated_at_ms = NowMs();
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

bool CodeAgentManager::SendMessage(const std::string& ns, const std::string& session_id, const std::string& text,
                                   const std::string& local_id, const nlohmann::json& attachments,
                                   std::string* error) {
  if (util::Trim(text).empty() && (!attachments.is_array() || attachments.empty())) {
    if (error != nullptr) {
      *error = "message requires text or attachments";
    }
    return false;
  }

  std::uint64_t generation = 0;
  bool should_start_run = false;
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
    session.active = true;
    session.updated_at_ms = NowMs();

    nlohmann::json content = {
        {"role", "user"},
        {"content", {
                        {"type", "text"},
                        {"text", text},
                    }},
        {"meta", {{"sentFrom", "webapp"}}},
    };
    if (attachments.is_array() && !attachments.empty()) {
      content["content"]["attachments"] = attachments;
    }

    bool inserted_new = false;
    const MessageRecord user_message =
        UpsertMessageLocked(session, std::move(content), local_id, /*dedupe_by_local_id=*/true, &inserted_new);
    PublishMessageReceivedLocked(session, user_message);

    if (inserted_new) {
      session.thinking = true;
      session.thinking_at_ms = NowMs();
      session.generation += 1;
      generation = session.generation;
      should_start_run = true;

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
  }

  if (should_start_run) {
    StartAgentRun(session_id, generation, text);
  }
  return true;
}

}  // namespace ferryman::codeagent
