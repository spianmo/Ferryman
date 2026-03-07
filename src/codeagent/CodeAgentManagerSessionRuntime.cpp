#include "ferryman/codeagent/CodeAgentManager.hpp"

#include "CodeAgentPolicy.hpp"
#include "ferryman/util/Random.hpp"
#include "ferryman/util/StringUtil.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace ferryman::codeagent {

namespace {

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool EndsWith(std::string_view value, std::string_view suffix) {
  if (suffix.size() > value.size()) {
    return false;
  }
  return value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsPermissionSelectorToolName(std::string_view name) {
  std::string normalized = ToLower(util::Trim(std::string(name)));
  if (normalized.empty()) {
    return false;
  }

  if (normalized == "askuserquestion" || normalized == "ask_user_question" || normalized == "request_user_input") {
    return true;
  }
  if (EndsWith(normalized, "__ask_user_question") || EndsWith(normalized, "_ask_user_question") ||
      EndsWith(normalized, "__request_user_input") || EndsWith(normalized, "_request_user_input")) {
    return true;
  }
  return false;
}

bool IsBashLikeToolName(std::string_view name) {
  const std::string normalized = ToLower(util::Trim(std::string(name)));
  return normalized == "bash" || normalized == "codexbash";
}

bool IsEditLikeToolName(std::string_view name) {
  const std::string normalized = ToLower(util::Trim(std::string(name)));
  if (normalized.empty()) {
    return false;
  }
  static constexpr std::array<std::string_view, 8> kEditHints = {
      "edit",
      "write",
      "patch",
      "create",
      "delete",
      "remove",
      "replace",
      "multiedit",
  };
  for (std::string_view hint : kEditHints) {
    if (normalized.find(hint) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> ReadToolCommand(const nlohmann::json& input) {
  if (!input.is_object()) {
    return std::nullopt;
  }
  static constexpr std::array<std::string_view, 2> kKeys = {"command", "cmd"};
  for (std::string_view key : kKeys) {
    auto it = input.find(std::string(key));
    if (it == input.end() || !it->is_string()) {
      continue;
    }
    const std::string command = util::Trim(it->get<std::string>());
    if (!command.empty()) {
      return command;
    }
  }
  return std::nullopt;
}

std::optional<std::string> BuildPermissionToolIdentifier(const std::string& tool_name, const nlohmann::json& input) {
  const std::string normalized_tool = util::Trim(tool_name);
  if (normalized_tool.empty()) {
    return std::nullopt;
  }
  if (IsBashLikeToolName(normalized_tool)) {
    if (auto command = ReadToolCommand(input); command.has_value()) {
      return "Bash(" + *command + ")";
    }
    return std::string("Bash");
  }
  return normalized_tool;
}

bool IsAllowToolPatternMatched(const std::string& allow_tool, const std::string& request_tool,
                               const nlohmann::json& request_input) {
  const std::string pattern = util::Trim(allow_tool);
  if (pattern.empty()) {
    return false;
  }

  if (pattern == "Bash" && IsBashLikeToolName(request_tool)) {
    return true;
  }

  if (pattern.size() > 6 && pattern.rfind("Bash(", 0) == 0 && pattern.back() == ')') {
    if (!IsBashLikeToolName(request_tool)) {
      return false;
    }
    auto command = ReadToolCommand(request_input);
    if (!command.has_value()) {
      return false;
    }
    std::string body = pattern.substr(5, pattern.size() - 6);
    if (body.size() > 2 && EndsWith(body, ":*")) {
      body.resize(body.size() - 2);
      return command->rfind(body, 0) == 0;
    }
    return *command == body;
  }

  return ToLower(pattern) == ToLower(util::Trim(request_tool));
}

bool IsPermissionAllowedForSession(const std::unordered_set<std::string>& allow_tools, const std::string& request_tool,
                                   const nlohmann::json& request_input) {
  if (allow_tools.empty()) {
    return false;
  }
  for (const auto& allow_tool : allow_tools) {
    if (IsAllowToolPatternMatched(allow_tool, request_tool, request_input)) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> ResolveAutoPermissionDecisionForMode(const std::string& mode,
                                                                const std::string& request_tool) {
  if (mode == "yolo" || mode == "bypassPermissions") {
    return std::string("approved_for_session");
  }
  if (mode == "safe-yolo") {
    return std::string("approved");
  }
  if (mode == "acceptEdits" && IsEditLikeToolName(request_tool)) {
    return std::string("approved");
  }
  return std::nullopt;
}

std::optional<std::string> ResolveAutoPermissionDecision(const std::unordered_set<std::string>& allow_tools,
                                                         const std::string& mode, const std::string& request_tool,
                                                         const nlohmann::json& request_input) {
  if (IsPermissionAllowedForSession(allow_tools, request_tool, request_input)) {
    return std::string("approved_for_session");
  }
  return ResolveAutoPermissionDecisionForMode(mode, request_tool);
}

}  // namespace

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
  std::string followup_text;
  bool should_continue = false;

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

    std::vector<std::string> auto_approved_lines;
    if (session.agent_state_requests.is_object()) {
      if (!session.agent_state_completed_requests.is_object()) {
        session.agent_state_completed_requests = nlohmann::json::object();
      }

      for (auto request_it = session.agent_state_requests.begin(); request_it != session.agent_state_requests.end();) {
        if (!request_it->is_object()) {
          ++request_it;
          continue;
        }

        const nlohmann::json request = *request_it;
        const std::string request_id = request_it.key();
        const std::string request_tool = request.value("tool", std::string("Tool"));
        const nlohmann::json request_arguments = request.value("arguments", nlohmann::json::object());

        if (IsPermissionSelectorToolName(request_tool)) {
          ++request_it;
          continue;
        }

        auto decision = ResolveAutoPermissionDecision(session.permission_allow_tools, normalized, request_tool,
                                                      request_arguments);
        if (!decision.has_value()) {
          ++request_it;
          continue;
        }

        const std::int64_t now = NowMs();
        request_it = session.agent_state_requests.erase(request_it);

        nlohmann::json completed = {
            {"tool", request_tool},
            {"arguments", request_arguments},
            {"createdAt", request.value("createdAt", now)},
            {"completedAt", now},
            {"status", "approved"},
            {"decision", *decision},
        };
        if (normalized != "default") {
          completed["mode"] = normalized;
        }
        if (*decision == "approved_for_session") {
          if (auto inferred = BuildPermissionToolIdentifier(request_tool, request_arguments); inferred.has_value()) {
            completed["allowTools"] = nlohmann::json::array({*inferred});
            session.permission_allow_tools.insert(*inferred);
          }
        }
        session.agent_state_completed_requests[request_id] = std::move(completed);
        session.updated_at_ms = std::max(session.updated_at_ms, now);

        auto_approved_lines.push_back("- " + request_tool + " (" + *decision + ")");
      }
    }

    if (!auto_approved_lines.empty()) {
      followup_text = "Permission mode switched to " + normalized + ".\nPending permission requests already resolved:\n";
      for (const auto& line : auto_approved_lines) {
        followup_text += line + "\n";
      }
      followup_text += "Continue the same conversation with those approvals already applied.";
      should_continue = true;
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
  }

  if (should_continue && !followup_text.empty()) {
    ContinueSessionWithInternalContext(session_id, std::move(followup_text));
  }
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

void CodeAgentManager::ContinueSessionWithInternalContext(const std::string& session_id,
                                                          std::string continuation_context) {
  if (util::Trim(continuation_context).empty()) {
    return;
  }

  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto session_it = sessions_by_id_.find(session_id);
    if (session_it == sessions_by_id_.end()) {
      return;
    }

    SessionRecord& session = session_it->second;
    session.active = true;
    session.thinking = true;
    session.thinking_at_ms = NowMs();
    session.updated_at_ms = session.thinking_at_ms;
    session.generation += 1;
    generation = session.generation;

    PushEventLocked(session.ns,
                    {
                        {"type", "session-updated"},
                        {"namespace", session.ns},
                        {"sessionId", session.id},
                        {"data", BuildSessionJsonLocked(session)},
                    },
                    session.id);
    PersistStateLocked();
  }

  StartAgentRun(session_id, generation, std::string(), std::move(continuation_context));
}

}  // namespace ferryman::codeagent
