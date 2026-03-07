#include "ferryman/codeagent/CodeAgentManager.hpp"

#include "CodeAgentPolicy.hpp"
#include "ferryman/util/Random.hpp"
#include "ferryman/util/StringUtil.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <regex>
#include <string_view>

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

std::string DetectHostTag() {
#if defined(__linux__)
  return "linux";
#elif defined(__APPLE__)
  return "darwin";
#elif defined(_WIN32)
  return "windows";
#elif defined(__FreeBSD__)
  return "freebsd";
#else
  return "unknown";
#endif
}

std::string EnvOrDefault(const char* key, const std::string& fallback) {
  const char* value = std::getenv(key);
  if (value == nullptr) {
    return fallback;
  }
  std::string trimmed = util::Trim(value);
  return trimmed.empty() ? fallback : trimmed;
}

std::string MakeCodexObjectId() {
  return "codex-" + util::RandomHex(18);
}

std::optional<std::string> JsonFirstString(const nlohmann::json& object, std::initializer_list<const char*> keys) {
  if (!object.is_object()) {
    return std::nullopt;
  }
  for (const char* key : keys) {
    if (key == nullptr) {
      continue;
    }
    auto it = object.find(key);
    if (it != object.end() && it->is_string()) {
      return it->get<std::string>();
    }
  }
  return std::nullopt;
}

std::optional<bool> JsonFirstBool(const nlohmann::json& object, std::initializer_list<const char*> keys) {
  if (!object.is_object()) {
    return std::nullopt;
  }
  for (const char* key : keys) {
    if (key == nullptr) {
      continue;
    }
    auto it = object.find(key);
    if (it != object.end() && it->is_boolean()) {
      return it->get<bool>();
    }
  }
  return std::nullopt;
}

const nlohmann::json* JsonObjectField(const nlohmann::json& object, const char* key) {
  if (!object.is_object() || key == nullptr) {
    return nullptr;
  }
  auto it = object.find(key);
  if (it == object.end() || !it->is_object()) {
    return nullptr;
  }
  return &(*it);
}

std::optional<std::string> ExtractCallId(const nlohmann::json& object) {
  return JsonFirstString(object, {"call_id", "callId", "tool_call_id", "toolCallId", "id"});
}

nlohmann::json ParseMaybeJsonString(const nlohmann::json& value) {
  if (!value.is_string()) {
    return value;
  }
  const std::string raw = util::Trim(value.get<std::string>());
  if (raw.empty()) {
    return value;
  }
  if ((raw.front() != '{' || raw.back() != '}') && (raw.front() != '[' || raw.back() != ']')) {
    return value;
  }
  try {
    return nlohmann::json::parse(raw);
  } catch (...) {
    return value;
  }
}

std::optional<std::string> ExtractTitleFromTitleRecord(const nlohmann::json& value) {
  if (!value.is_object()) {
    return std::nullopt;
  }

  static constexpr std::array<std::string_view, 7> kTitleKeys = {
      "title",
      "name",
      "new_title",
      "session_title",
      "newTitle",
      "sessionTitle",
      "chat_title",
  };

  for (std::string_view key : kTitleKeys) {
    auto it = value.find(std::string(key));
    if (it == value.end() || !it->is_string()) {
      continue;
    }
    std::string title = util::Trim(it->get<std::string>());
    if (!title.empty()) {
      return title;
    }
  }
  return std::nullopt;
}

std::optional<std::string> ExtractTitleFromTitleToolInput(const nlohmann::json& input, int depth = 0) {
  if (depth > 4) {
    return std::nullopt;
  }

  if (input.is_object()) {
    if (auto direct = ExtractTitleFromTitleRecord(input); direct.has_value()) {
      return direct;
    }

    static constexpr std::array<std::string_view, 4> kNestedKeys = {
        "arguments",
        "input",
        "params",
        "data",
    };
    for (std::string_view key : kNestedKeys) {
      auto it = input.find(std::string(key));
      if (it == input.end()) {
        continue;
      }
      if (auto nested = ExtractTitleFromTitleToolInput(*it, depth + 1); nested.has_value()) {
        return nested;
      }
    }
    return std::nullopt;
  }

  if (input.is_array()) {
    for (const auto& entry : input) {
      if (auto nested = ExtractTitleFromTitleToolInput(entry, depth + 1); nested.has_value()) {
        return nested;
      }
    }
    return std::nullopt;
  }

  if (input.is_string()) {
    nlohmann::json parsed = ParseMaybeJsonString(input);
    if (parsed.is_object() || parsed.is_array()) {
      return ExtractTitleFromTitleToolInput(parsed, depth + 1);
    }
  }
  return std::nullopt;
}

std::optional<std::string> ExtractQuotedTitleFromText(const std::string& text) {
  const std::string trimmed = util::Trim(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  static const std::regex kQuotedTitlePattern(
      R"TITLE((?:title|chat title|session title|标题)[^"\n\r]{0,64}"([^"\n\r]{1,200})")TITLE",
      std::regex::icase);
  std::smatch match;
  if (std::regex_search(trimmed, match, kQuotedTitlePattern) && match.size() >= 2) {
    const std::string title = util::Trim(match[1].str());
    if (!title.empty()) {
      return title;
    }
  }

  // Common shape: `Successfully changed chat title to: "..."`.
  const size_t first_quote = trimmed.find('"');
  if (first_quote == std::string::npos) {
    return std::nullopt;
  }
  const size_t second_quote = trimmed.find('"', first_quote + 1);
  if (second_quote == std::string::npos || second_quote <= first_quote + 1) {
    return std::nullopt;
  }
  const std::string title = util::Trim(trimmed.substr(first_quote + 1, second_quote - first_quote - 1));
  if (!title.empty()) {
    return title;
  }
  return std::nullopt;
}

std::optional<std::string> ExtractTitleFromChangeTitleResult(const nlohmann::json& output) {
  if (auto direct = ExtractTitleFromTitleToolInput(output); direct.has_value()) {
    return direct;
  }

  if (output.is_string()) {
    return ExtractQuotedTitleFromText(output.get<std::string>());
  }

  if (!output.is_object()) {
    return std::nullopt;
  }

  if (auto text = JsonFirstString(output, {"message", "text", "content", "result"}); text.has_value()) {
    if (auto extracted = ExtractQuotedTitleFromText(*text); extracted.has_value()) {
      return extracted;
    }
  }

  auto content_it = output.find("content");
  if (content_it != output.end() && content_it->is_array()) {
    std::string joined_text;
    for (const auto& part : *content_it) {
      if (!part.is_object()) {
        continue;
      }
      auto text = JsonFirstString(part, {"text", "message", "content"});
      if (!text.has_value()) {
        continue;
      }
      joined_text += *text;
      if (auto extracted = ExtractQuotedTitleFromText(*text); extracted.has_value()) {
        return extracted;
      }
    }
    if (!joined_text.empty()) {
      if (auto extracted = ExtractQuotedTitleFromText(joined_text); extracted.has_value()) {
        return extracted;
      }
    }
  }
  return std::nullopt;
}

bool IsChangeTitleToolName(std::string_view name) {
  std::string normalized = ToLower(util::Trim(std::string(name)));
  if (normalized.empty()) {
    return false;
  }

  static constexpr std::string_view kFunctionsPrefix = "functions.";
  if (normalized.rfind(kFunctionsPrefix, 0) == 0) {
    normalized.erase(0, kFunctionsPrefix.size());
  }

  for (char& ch : normalized) {
    if (ch == '-' || ch == '.' || ch == '/' || ch == ':' || ch == ' ') {
      ch = '_';
    }
  }

  if (normalized == "mcp__hapi__change_title" || normalized == "hapi__change_title" ||
      normalized == "hapi_change_title" || normalized == "happy__change_title" || normalized == "change_title") {
    return true;
  }

  if (EndsWith(normalized, "__change_title") || EndsWith(normalized, "_change_title")) {
    return true;
  }

  return normalized.find("change_title") != std::string::npos;
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

bool IsPermissionToolName(std::string_view name) {
  std::string normalized = ToLower(util::Trim(std::string(name)));
  if (normalized.empty()) {
    return false;
  }
  if (normalized == "codexpermission" || normalized == "codex_permission" || normalized == "permission" ||
      normalized == "codexpermissionrequest" || normalized == "codex_permission_request" ||
      normalized == "codexbash" || IsPermissionSelectorToolName(normalized)) {
    return true;
  }
  if (normalized.find("permission") != std::string::npos) {
    return true;
  }
  return false;
}

bool IsPermissionRequestInput(const nlohmann::json& input) {
  if (!input.is_object()) {
    return false;
  }
  if (JsonFirstBool(input, {"approval_request", "approvalRequest", "requires_approval", "requiresApproval"})
          .value_or(false)) {
    return true;
  }
  if (input.contains("questions") || input.contains("allow_tools") || input.contains("allowTools")) {
    return true;
  }
  if (auto auto_approved = JsonFirstBool(input, {"auto_approved", "autoApproved"}); auto_approved.has_value()) {
    return !*auto_approved;
  }
  return false;
}

bool IsKnownPermissionDecision(std::string_view decision) {
  return decision == "approved" || decision == "approved_for_session" || decision == "denied" || decision == "abort";
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
  auto command = JsonFirstString(input, {"command", "cmd"});
  if (!command.has_value()) {
    return std::nullopt;
  }
  const std::string normalized = util::Trim(*command);
  if (normalized.empty()) {
    return std::nullopt;
  }
  return normalized;
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

void MergePermissionAllowTools(std::unordered_set<std::string>* target, const std::vector<std::string>& allow_tools) {
  if (target == nullptr) {
    return;
  }
  for (const auto& tool : allow_tools) {
    const std::string normalized = util::Trim(tool);
    if (!normalized.empty()) {
      target->insert(normalized);
    }
  }
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

std::optional<std::string> ResolveAutoPermissionDecisionForMode(const std::string& permission_mode,
                                                                const std::string& request_tool) {
  const std::string mode = policy::NormalizePermissionModeValue(permission_mode);
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
                                                         const std::string& permission_mode,
                                                         const std::string& request_tool,
                                                         const nlohmann::json& request_input) {
  if (IsPermissionAllowedForSession(allow_tools, request_tool, request_input)) {
    return std::string("approved_for_session");
  }
  return ResolveAutoPermissionDecisionForMode(permission_mode, request_tool);
}

bool HasAnsweredSelectorRequest(const nlohmann::json& completed_requests, const std::string& request_tool,
                                const nlohmann::json& request_input) {
  if (!completed_requests.is_object()) {
    return false;
  }

  for (auto it = completed_requests.begin(); it != completed_requests.end(); ++it) {
    if (!it->is_object()) {
      continue;
    }

    const nlohmann::json& completed = *it;
    if (util::Trim(completed.value("tool", std::string())) != util::Trim(request_tool)) {
      continue;
    }
    if (completed.value("status", std::string()) != "approved") {
      continue;
    }

    auto answers_it = completed.find("answers");
    if (answers_it == completed.end() || !answers_it->is_object() || answers_it->empty()) {
      continue;
    }

    auto arguments_it = completed.find("arguments");
    const nlohmann::json completed_arguments =
        arguments_it != completed.end() ? *arguments_it : nlohmann::json::object();
    if (completed_arguments == request_input) {
      return true;
    }
  }

  return false;
}

nlohmann::json SerializePermissionAllowTools(const std::unordered_set<std::string>& allow_tools) {
  nlohmann::json result = nlohmann::json::array();
  if (allow_tools.empty()) {
    return result;
  }
  std::vector<std::string> values(allow_tools.begin(), allow_tools.end());
  std::sort(values.begin(), values.end());
  for (const auto& value : values) {
    result.push_back(value);
  }
  return result;
}

}  // namespace

CodeAgentManager::CodeAgentManager(const core::AppConfig& config) {
  const std::int64_t now = NowMs();
  machine_.id = "machine-" + util::RandomHex(12);
  machine_.updated_at_ms = now;
  machine_.metadata = {
      {"host", "localhost"},
      {"platform", DetectHostTag()},
      {"happyCliVersion", "ferryman-codeagent-cpp"},
      {"displayName", "Ferryman Local Runner"},
  };

  claude_cmd_template_ =
      EnvOrDefault("FERRYMAN_CODEAGENT_CLAUDE_CMD", "claude --output-format stream-json --verbose -p {prompt}");
  codex_cmd_template_ = EnvOrDefault("FERRYMAN_CODEAGENT_CODEX_CMD", "codex exec --json {prompt}");
  cursor_cmd_template_ = EnvOrDefault("FERRYMAN_CODEAGENT_CURSOR_CMD", "agent -p {prompt} --output-format stream-json --trust");
  gemini_cmd_template_ = EnvOrDefault("FERRYMAN_CODEAGENT_GEMINI_CMD", "gemini -p {prompt}");
  opencode_cmd_template_ = EnvOrDefault("FERRYMAN_CODEAGENT_OPENCODE_CMD", "opencode {prompt}");

  if (!config.config_path.empty()) {
    const std::filesystem::path state_dir = config.config_path.parent_path();
    state_file_path_ = state_dir / "codeagent_sessions.db";
    legacy_state_file_path_ = state_dir / "codeagent_sessions.json";
  }
  if (!state_file_path_.empty()) {
    std::lock_guard<std::mutex> lock(mu_);
    RestoreStateLocked();
  }

  PushEventLocked("default",
                  {
                      {"type", "machine-updated"},
                      {"namespace", "default"},
                      {"machineId", machine_.id},
                      {"data", {"active", true, "metadata", machine_.metadata}},
                  },
                  "", machine_.id);
}

CodeAgentManager::~CodeAgentManager() {
  std::lock_guard<std::mutex> lock(mu_);
  PersistStateLocked();
}

std::int64_t CodeAgentManager::NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string CodeAgentManager::Trim(std::string value) {
  return util::Trim(value);
}

std::string CodeAgentManager::NormalizeAgent(std::string agent) {
  agent = ToLower(util::Trim(agent));
  if (agent == "claude" || agent == "codex" || agent == "cursor" || agent == "gemini" || agent == "opencode") {
    return agent;
  }
  return "claude";
}

nlohmann::json CodeAgentManager::BuildMessageJson(const MessageRecord& message) const {
  nlohmann::json local_id = nullptr;
  if (!message.local_id.empty()) {
    local_id = message.local_id;
  }
  return {
      {"id", message.id},
      {"seq", message.seq},
      {"localId", local_id},
      {"content", message.content},
      {"createdAt", message.created_at_ms},
  };
}

CodeAgentManager::MessageRecord CodeAgentManager::UpsertMessageLocked(SessionRecord& session, nlohmann::json content,
                                                                      const std::string& local_id,
                                                                      bool dedupe_by_local_id, bool* inserted_new) {
  if (inserted_new != nullptr) {
    *inserted_new = false;
  }

  const std::string normalized_local_id = util::Trim(local_id);
  if (dedupe_by_local_id && !normalized_local_id.empty()) {
    auto existing = std::find_if(session.messages.begin(), session.messages.end(),
                                 [&normalized_local_id](const MessageRecord& message) {
                                   return message.local_id == normalized_local_id;
                                 });
    if (existing != session.messages.end()) {
      return *existing;
    }
  }

  MessageRecord message;
  message.id = "msg-" + util::RandomHex(18);
  message.seq = static_cast<int>(session.messages.size() + 1);
  message.local_id = normalized_local_id;
  message.created_at_ms = NowMs();
  message.content = std::move(content);
  session.messages.push_back(message);
  if (inserted_new != nullptr) {
    *inserted_new = true;
  }
  return message;
}

void CodeAgentManager::PublishMessageReceivedLocked(const SessionRecord& session, const MessageRecord& message) {
  PushEventLocked(session.ns,
                  {
                      {"type", "message-received"},
                      {"namespace", session.ns},
                      {"sessionId", session.id},
                      {"message", BuildMessageJson(message)},
                  },
                  session.id);
}

void CodeAgentManager::EmitAgentEventMessageLocked(SessionRecord& session, nlohmann::json event_data) {
  if (!event_data.is_object()) {
    return;
  }
  auto event_type = JsonFirstString(event_data, {"type"});
  if (!event_type.has_value() || util::Trim(*event_type).empty()) {
    return;
  }

  nlohmann::json content = {
      {"role", "agent"},
      {"content", {
                      {"type", "event"},
                      {"data", std::move(event_data)},
                  }},
      {"meta", {{"sentFrom", "cli"}}},
  };

  bool inserted_new = false;
  const MessageRecord event_message =
      UpsertMessageLocked(session, std::move(content), "", /*dedupe_by_local_id=*/false, &inserted_new);
  (void)inserted_new;
  session.updated_at_ms = std::max(session.updated_at_ms, event_message.created_at_ms);
  PublishMessageReceivedLocked(session, event_message);
}

nlohmann::json CodeAgentManager::BuildSessionSummaryJsonLocked(const SessionRecord& session) const {
  const int pending_requests_count =
      session.agent_state_requests.is_object() ? static_cast<int>(session.agent_state_requests.size()) : 0;

  nlohmann::json summary = {
      {"id", session.id},
      {"active", session.active},
      {"thinking", session.thinking},
      {"activeAt", session.active_at_ms},
      {"updatedAt", session.updated_at_ms},
      {"metadata", {
                       {"name", session.name.empty() ? nlohmann::json(nullptr) : nlohmann::json(session.name)},
                       {"path", session.path.string()},
                       {"machineId", session.machine_id},
                       {"summary", session.summary_text.empty()
                                        ? nlohmann::json(nullptr)
                                        : nlohmann::json{{"text", session.summary_text}}},
                       {"flavor", session.flavor},
                       {"model", session.model.empty() ? nlohmann::json(nullptr) : nlohmann::json(session.model)},
                   }},
      {"todoProgress", nullptr},
      {"pendingRequestsCount", pending_requests_count},
      {"modelMode", session.model_mode},
  };
  if (session.flavor == "codex" && !session.model_reasoning_effort.empty()) {
    summary["reasoningEffort"] = session.model_reasoning_effort;
  }
  return summary;
}

nlohmann::json CodeAgentManager::BuildSessionJsonLocked(const SessionRecord& session) const {
  nlohmann::json metadata = {
      {"path", session.path.string()},
      {"host", session.host.empty() ? "localhost" : session.host},
      {"name", session.name.empty() ? nlohmann::json(nullptr) : nlohmann::json(session.name)},
      {"model", session.model.empty() ? nlohmann::json(nullptr) : nlohmann::json(session.model)},
      {"summary", session.summary_text.empty()
                      ? nlohmann::json(nullptr)
                      : nlohmann::json{{"text", session.summary_text}, {"updatedAt", session.summary_updated_at_ms}}},
      {"machineId", session.machine_id},
      {"flavor", session.flavor},
      {"version", "ferryman-codeagent-cpp"},
      {"os", DetectHostTag()},
  };

  nlohmann::json agent_state = {
      {"controlledByUser", true},
      {"requests", session.agent_state_requests.is_object() ? session.agent_state_requests : nlohmann::json::object()},
      {"completedRequests",
       session.agent_state_completed_requests.is_object() ? session.agent_state_completed_requests
                                                          : nlohmann::json::object()},
  };

  if (session.flavor == "codex" && !session.model_reasoning_effort.empty()) {
    metadata["reasoningEffort"] = session.model_reasoning_effort;
  }
  nlohmann::json result = {
      {"id", session.id},
      {"namespace", session.ns},
      {"seq", session.seq},
      {"createdAt", session.created_at_ms},
      {"updatedAt", session.updated_at_ms},
      {"active", session.active},
      {"activeAt", session.active_at_ms},
      {"metadata", metadata},
      {"metadataVersion", 1},
      {"agentState", agent_state},
      {"agentStateVersion", 1},
      {"thinking", session.thinking},
      {"thinkingAt", session.thinking_at_ms},
      {"todos", nlohmann::json::array()},
      {"permissionMode", session.permission_mode},
      {"modelMode", session.model_mode},
      {"permissionAllowTools", SerializePermissionAllowTools(session.permission_allow_tools)},
  };
  if (session.flavor == "codex" && !session.model_reasoning_effort.empty()) {
    result["reasoningEffort"] = session.model_reasoning_effort;
  }
  return result;
}

std::optional<CodeAgentManager::SessionRecord*> CodeAgentManager::GetMutableSessionLocked(const std::string& ns,
                                                                                           const std::string& session_id) {
  auto it = sessions_by_id_.find(session_id);
  if (it == sessions_by_id_.end()) {
    return std::nullopt;
  }
  if (it->second.ns != ns) {
    return std::nullopt;
  }
  return &it->second;
}

std::optional<const CodeAgentManager::SessionRecord*> CodeAgentManager::GetSessionLocked(const std::string& ns,
                                                                                          const std::string& session_id) const {
  auto it = sessions_by_id_.find(session_id);
  if (it == sessions_by_id_.end()) {
    return std::nullopt;
  }
  if (it->second.ns != ns) {
    return std::nullopt;
  }
  return &it->second;
}

void CodeAgentManager::PushEventLocked(const std::string& ns, const nlohmann::json& payload,
                                       const std::string& session_id, const std::string& machine_id) {
  events_.push_back(EventRecord{
      .id = next_event_id_++,
      .ns = ns,
      .session_id = session_id,
      .machine_id = machine_id,
      .payload = payload,
  });

  constexpr size_t kMaxEvents = 2048;
  while (events_.size() > kMaxEvents) {
    events_.pop_front();
  }
  event_cv_.notify_all();
}

bool CodeAgentManager::ApprovePermissionRequest(const std::string& ns, const std::string& session_id,
                                                const std::string& request_id, const std::string& mode,
                                                const std::vector<std::string>& allow_tools,
                                                const std::string& decision, const nlohmann::json& answers,
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
    if (!session.active) {
      if (error != nullptr) {
        *error = "session is inactive";
      }
      return false;
    }

    const std::string normalized_request_id = util::Trim(request_id);
    if (normalized_request_id.empty()) {
      if (error != nullptr) {
        *error = "request not found";
      }
      return false;
    }

    if (!session.agent_state_requests.is_object()) {
      session.agent_state_requests = nlohmann::json::object();
    }
    auto request_it = session.agent_state_requests.find(normalized_request_id);
    if (request_it == session.agent_state_requests.end() || !request_it->is_object()) {
      if (error != nullptr) {
        *error = "request not found";
      }
      return false;
    }

    const std::string trimmed_mode = util::Trim(mode);
    std::string normalized_mode =
        trimmed_mode.empty() ? std::string() : policy::NormalizePermissionModeValue(trimmed_mode);
    if (!trimmed_mode.empty() && normalized_mode.empty()) {
      if (error != nullptr) {
        *error = "invalid permission mode for session flavor";
      }
      return false;
    }
    if (!normalized_mode.empty() && !policy::IsPermissionModeAllowedForFlavor(normalized_mode, session.flavor)) {
      if (error != nullptr) {
        *error = "invalid permission mode for session flavor";
      }
      return false;
    }

    std::string normalized_decision = ToLower(util::Trim(decision));
    if (!normalized_decision.empty() && !IsKnownPermissionDecision(normalized_decision)) {
      if (error != nullptr) {
        *error = "invalid decision";
      }
      return false;
    }

    std::vector<std::string> effective_allow_tools;
    effective_allow_tools.reserve(allow_tools.size());
    std::unordered_set<std::string> allow_tool_dedup;
    for (const auto& raw_tool : allow_tools) {
      const std::string normalized_tool = util::Trim(raw_tool);
      if (normalized_tool.empty()) {
        continue;
      }
      if (allow_tool_dedup.insert(normalized_tool).second) {
        effective_allow_tools.push_back(normalized_tool);
      }
    }

    nlohmann::json request = *request_it;
    session.agent_state_requests.erase(request_it);

    if (!session.agent_state_completed_requests.is_object()) {
      session.agent_state_completed_requests = nlohmann::json::object();
    }

    const std::int64_t now = NowMs();
    const std::string request_tool = request.value("tool", std::string("Tool"));
    const nlohmann::json request_arguments = request.value("arguments", nlohmann::json::object());
    if (normalized_decision == "approved_for_session" && effective_allow_tools.empty()) {
      if (auto inferred = BuildPermissionToolIdentifier(request_tool, request_arguments); inferred.has_value()) {
        effective_allow_tools.push_back(*inferred);
      }
    }
    if (!effective_allow_tools.empty()) {
      MergePermissionAllowTools(&session.permission_allow_tools, effective_allow_tools);
    }
    nlohmann::json completed = {
        {"tool", request_tool},
        {"arguments", request_arguments},
        {"createdAt", request.value("createdAt", now)},
        {"completedAt", now},
        {"status", "approved"},
    };
    if (!normalized_mode.empty() && normalized_mode != "default") {
      completed["mode"] = normalized_mode;
    }
    if (!effective_allow_tools.empty()) {
      completed["allowTools"] = effective_allow_tools;
    }
    if (!normalized_decision.empty()) {
      completed["decision"] = normalized_decision;
    }
    if (answers.is_object() && !answers.empty()) {
      completed["answers"] = answers;
    }

    const bool has_mode_override = !normalized_mode.empty();
    const bool mode_changed = has_mode_override && session.permission_mode != normalized_mode;
    if (has_mode_override) {
      session.permission_mode = normalized_mode;
      if (session.flavor == "claude") {
        session.yolo = normalized_mode == "bypassPermissions";
      } else if (policy::IsCodexOrGeminiFlavor(session.flavor) || session.flavor == "cursor" ||
                 session.flavor == "opencode") {
        session.yolo = normalized_mode == "yolo";
      } else {
        session.yolo = false;
      }
      if (mode_changed) {
        EmitAgentEventMessageLocked(session,
                                    {
                                        {"type", "permission-mode-changed"},
                                        {"mode", normalized_mode},
                                    });
      }
    }

    session.agent_state_completed_requests[normalized_request_id] = std::move(completed);
    session.updated_at_ms = now;

    if (IsPermissionSelectorToolName(request_tool) || IsPermissionToolName(request_tool) ||
        IsPermissionRequestInput(request_arguments)) {
      const auto safe_dump = [](const nlohmann::json& value) {
        return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
      };
      if (answers.is_object() && !answers.empty()) {
        followup_text = "Pending request resolved.\nTool: " + request_tool +
                        "\nDecision: approved\nAnswers: " + safe_dump(answers) +
                        "\nTreat the answers as already-submitted user input and continue the same conversation.";
      } else if (!normalized_decision.empty()) {
        followup_text = "Pending request resolved.\nTool: " + request_tool +
                        "\nDecision: " + normalized_decision +
                        "\nContinue the same conversation with that approval already applied.";
      } else {
        followup_text = "Pending request resolved.\nTool: " + request_tool +
                        "\nDecision: approved\nContinue the same conversation with that approval already applied.";
      }
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

bool CodeAgentManager::DenyPermissionRequest(const std::string& ns, const std::string& session_id,
                                             const std::string& request_id, const std::string& decision,
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
    if (!session.active) {
      if (error != nullptr) {
        *error = "session is inactive";
      }
      return false;
    }

    const std::string normalized_request_id = util::Trim(request_id);
    if (normalized_request_id.empty()) {
      if (error != nullptr) {
        *error = "request not found";
      }
      return false;
    }

    if (!session.agent_state_requests.is_object()) {
      session.agent_state_requests = nlohmann::json::object();
    }
    auto request_it = session.agent_state_requests.find(normalized_request_id);
    if (request_it == session.agent_state_requests.end() || !request_it->is_object()) {
      if (error != nullptr) {
        *error = "request not found";
      }
      return false;
    }

    std::string normalized_decision = ToLower(util::Trim(decision));
    if (!normalized_decision.empty() && !IsKnownPermissionDecision(normalized_decision)) {
      if (error != nullptr) {
        *error = "invalid decision";
      }
      return false;
    }

    nlohmann::json request = *request_it;
    session.agent_state_requests.erase(request_it);

    if (!session.agent_state_completed_requests.is_object()) {
      session.agent_state_completed_requests = nlohmann::json::object();
    }

    const std::int64_t now = NowMs();
    const std::string request_tool = request.value("tool", std::string("Tool"));
    const nlohmann::json request_arguments = request.value("arguments", nlohmann::json::object());
    const std::string deny_reason = normalized_decision == "abort" ? "Aborted by user" : "Denied by user";
    nlohmann::json completed = {
        {"tool", request_tool},
        {"arguments", request_arguments},
        {"createdAt", request.value("createdAt", now)},
        {"completedAt", now},
        {"status", "denied"},
        {"reason", deny_reason},
    };
    if (!normalized_decision.empty()) {
      completed["decision"] = normalized_decision;
    }

    session.agent_state_completed_requests[normalized_request_id] = std::move(completed);
    session.updated_at_ms = now;

    if (IsPermissionSelectorToolName(request_tool) || IsPermissionToolName(request_tool) ||
        IsPermissionRequestInput(request_arguments)) {
      const std::string decision_text = normalized_decision.empty() ? std::string("denied") : normalized_decision;
      followup_text = "Pending request resolved.\nTool: " + request_tool + "\nDecision: " + decision_text +
                      "\nReason: " + deny_reason +
                      "\nContinue the same conversation and take an alternative approach if appropriate.";
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

bool CodeAgentManager::EmitCodexBodyMessage(const std::string& session_id, std::uint64_t generation,
                                            const nlohmann::json& body, std::string* summary_text) {
  if (!body.is_object()) {
    return false;
  }

  nlohmann::json normalized = body;
  if (!JsonFirstString(normalized, {"id"}).has_value()) {
    normalized["id"] = MakeCodexObjectId();
  }

  if (summary_text != nullptr) {
    summary_text->clear();
    const auto type = JsonFirstString(normalized, {"type"});
    if (type.has_value() && *type == "message") {
      auto message = JsonFirstString(normalized, {"message"});
      if (message.has_value()) {
        *summary_text = *message;
      }
    } else if (type.has_value() && *type == "reasoning") {
      auto message = JsonFirstString(normalized, {"message"});
      if (message.has_value()) {
        *summary_text = *message;
      }
    } else if (type.has_value() && *type == "tool-call") {
      auto name = JsonFirstString(normalized, {"name"});
      if (name.has_value()) {
        *summary_text = "Tool: " + *name;
      }
    }
  }

  std::optional<std::string> title_change;
  bool has_title_signal = false;
  const auto body_type = JsonFirstString(normalized, {"type"});
  if (body_type.has_value() && *body_type == "tool-call") {
    auto tool_name = JsonFirstString(normalized, {"name"});
    if (tool_name.has_value() && IsChangeTitleToolName(*tool_name)) {
      has_title_signal = true;
      auto input_it = normalized.find("input");
      if (input_it != normalized.end()) {
        title_change = ExtractTitleFromTitleToolInput(*input_it);
      }
      if (!title_change.has_value()) {
        auto arguments_it = normalized.find("arguments");
        if (arguments_it != normalized.end()) {
          title_change = ExtractTitleFromTitleToolInput(*arguments_it);
        }
      }
    }
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto session_it = sessions_by_id_.find(session_id);
  if (session_it == sessions_by_id_.end()) {
    return false;
  }

  SessionRecord& session = session_it->second;
  if (session.generation != generation) {
    return false;
  }

  bool permission_state_changed = false;
  bool should_publish_session_update = false;
  bool suppress_body_message = false;

  if (body_type.has_value() && *body_type == "tool-call-result") {
    bool title_tool_result = false;
    std::optional<std::string> title_from_call_input;

    auto explicit_tool_name = JsonFirstString(normalized, {"name", "tool", "toolName", "tool_name"});
    if (explicit_tool_name.has_value() && IsChangeTitleToolName(*explicit_tool_name)) {
      title_tool_result = true;
    }

    auto call_id = JsonFirstString(normalized, {"callId"});
    if (call_id.has_value() && !call_id->empty()) {
      for (auto message_it = session.messages.rbegin(); message_it != session.messages.rend(); ++message_it) {
        if (!message_it->content.is_object()) {
          continue;
        }
        const nlohmann::json* wrapped_content = JsonObjectField(message_it->content, "content");
        if (wrapped_content == nullptr || !wrapped_content->is_object()) {
          continue;
        }
        if (JsonFirstString(*wrapped_content, {"type"}).value_or("") != "codex") {
          continue;
        }
        const nlohmann::json* data = JsonObjectField(*wrapped_content, "data");
        if (data == nullptr || !data->is_object()) {
          continue;
        }
        if (JsonFirstString(*data, {"type"}).value_or("") != "tool-call") {
          continue;
        }
        auto tool_call_id = JsonFirstString(*data, {"callId"});
        if (!tool_call_id.has_value() || *tool_call_id != *call_id) {
          continue;
        }

        auto tool_name = JsonFirstString(*data, {"name"});
        if (!tool_name.has_value() || !IsChangeTitleToolName(*tool_name)) {
          break;
        }

        title_tool_result = true;
        auto input_it = data->find("input");
        if (input_it != data->end()) {
          title_from_call_input = ExtractTitleFromTitleToolInput(*input_it);
        }
        break;
      }
    }

    if (title_tool_result) {
      has_title_signal = true;
      auto output_it = normalized.find("output");
      if (output_it != normalized.end()) {
        title_change = ExtractTitleFromChangeTitleResult(*output_it);
      }
      if (!title_change.has_value() && title_from_call_input.has_value()) {
        title_change = title_from_call_input;
      }
      if (!title_change.has_value()) {
        auto input_it = normalized.find("input");
        if (input_it != normalized.end()) {
          title_change = ExtractTitleFromTitleToolInput(*input_it);
        }
      }
      if (!title_change.has_value()) {
        auto arguments_it = normalized.find("arguments");
        if (arguments_it != normalized.end()) {
          title_change = ExtractTitleFromTitleToolInput(*arguments_it);
        }
      }
    }
  }

  if (body_type.has_value() && *body_type == "tool-call") {
    const auto call_id = JsonFirstString(normalized, {"callId"});
    const auto tool_name = JsonFirstString(normalized, {"name"});
    nlohmann::json input = nlohmann::json::object();
    if (auto input_it = normalized.find("input"); input_it != normalized.end()) {
      input = *input_it;
    } else if (auto args_it = normalized.find("arguments"); args_it != normalized.end()) {
      input = *args_it;
    }

    bool should_track_permission = false;
    if (tool_name.has_value() && !tool_name->empty()) {
      const bool explicit_permission_request = IsPermissionRequestInput(input);
      if (IsPermissionSelectorToolName(*tool_name)) {
        should_track_permission = true;
      } else if (explicit_permission_request &&
                 (IsPermissionToolName(*tool_name) || IsBashLikeToolName(*tool_name) || IsEditLikeToolName(*tool_name))) {
        should_track_permission = true;
      } else if (ToLower(*tool_name) == "codexpermission" || ToLower(*tool_name) == "codex_permission") {
        should_track_permission = true;
      }
    }

    if (should_track_permission && call_id.has_value() && !call_id->empty()) {
      if (!session.agent_state_requests.is_object()) {
        session.agent_state_requests = nlohmann::json::object();
      }
      if (!session.agent_state_completed_requests.is_object()) {
        session.agent_state_completed_requests = nlohmann::json::object();
      }

      const std::string request_tool = tool_name.value_or("Tool");
      const bool selector_request = IsPermissionSelectorToolName(request_tool);

      if (selector_request &&
          HasAnsweredSelectorRequest(session.agent_state_completed_requests, request_tool, input)) {
        session.suppressed_permission_call_ids.insert(*call_id);
        suppress_body_message = true;
      } else {
        bool has_other_selector_pending = false;
        if (selector_request && session.agent_state_requests.is_object()) {
          for (auto it = session.agent_state_requests.begin(); it != session.agent_state_requests.end(); ++it) {
            if (it.key() == *call_id || !it.value().is_object()) {
              continue;
            }
            const std::string pending_tool = it.value().value("tool", std::string("Tool"));
            if (IsPermissionSelectorToolName(pending_tool)) {
              has_other_selector_pending = true;
              break;
            }
          }
        }

        if (has_other_selector_pending) {
          session.suppressed_permission_call_ids.insert(*call_id);
          suppress_body_message = true;
        } else {
          const std::int64_t now = NowMs();
          auto auto_decision =
              selector_request ? std::optional<std::string>()
                               : ResolveAutoPermissionDecision(session.permission_allow_tools, session.permission_mode,
                                                               request_tool, input);
          if (auto_decision.has_value()) {
            nlohmann::json completed = {
                {"tool", request_tool},
                {"arguments", input},
                {"createdAt", now},
                {"completedAt", now},
                {"status",
                 *auto_decision == "approved" || *auto_decision == "approved_for_session" ? "approved" : "denied"},
                {"decision", *auto_decision},
            };
            if (*auto_decision == "approved_for_session") {
              if (auto inferred = BuildPermissionToolIdentifier(request_tool, input); inferred.has_value()) {
                completed["allowTools"] = nlohmann::json::array({*inferred});
                session.permission_allow_tools.insert(*inferred);
              }
            }
            session.agent_state_completed_requests[*call_id] = std::move(completed);
            session.updated_at_ms = std::max(session.updated_at_ms, now);
            permission_state_changed = true;
            should_publish_session_update = true;
          } else {
            session.agent_state_requests[*call_id] = {
                {"tool", request_tool},
                {"arguments", input},
                {"createdAt", now},
            };
            session.agent_state_completed_requests.erase(*call_id);
            session.updated_at_ms = std::max(session.updated_at_ms, now);
            permission_state_changed = true;
            should_publish_session_update = true;

            session.thinking = false;
            session.generation += 1;

            const std::string short_title = selector_request ? "Input Needed" : "Permission Required";
            const std::string short_body = selector_request ? "The agent is waiting for your selection."
                                                            : "The agent is waiting for your approval.";
            PushEventLocked(session.ns,
                            {
                                {"type", "toast"},
                                {"namespace", session.ns},
                                {"data",
                                 {
                                     {"title", short_title},
                                     {"body", short_body},
                                     {"sessionId", session.id},
                                     {"url", "/sessions/" + session.id},
                                 }},
                            },
                            session.id);
          }
        }
      }
    }
  } else if (body_type.has_value() && *body_type == "tool-call-result") {
    const auto call_id = JsonFirstString(normalized, {"callId"});
    if (call_id.has_value() && !call_id->empty()) {
      if (session.suppressed_permission_call_ids.erase(*call_id) > 0) {
        suppress_body_message = true;
      }
    }
    if (call_id.has_value() && !call_id->empty() && session.agent_state_requests.is_object()) {
      auto request_it = session.agent_state_requests.find(*call_id);
      if (request_it != session.agent_state_requests.end() && request_it->is_object()) {
        const nlohmann::json request = *request_it;
        const nlohmann::json* output = JsonObjectField(normalized, "output");
        const std::string request_tool = request.value("tool", std::string("Tool"));
        const nlohmann::json request_arguments = request.value("arguments", nlohmann::json::object());

        std::string decision = ToLower(util::Trim(JsonFirstString(normalized, {"decision"}).value_or("")));
        if (decision.empty() && output != nullptr) {
          decision = ToLower(util::Trim(JsonFirstString(*output, {"decision"}).value_or("")));
        }
        if (!decision.empty() && !IsKnownPermissionDecision(decision)) {
          decision.clear();
        }

        std::optional<bool> approved;
        if (decision.empty()) {
          approved = JsonFirstBool(normalized, {"approved"});
          if (!approved.has_value() && output != nullptr) {
            approved = JsonFirstBool(*output, {"approved"});
          }
          if (approved.has_value()) {
            decision = *approved ? "approved" : "denied";
          }
        }

        if (decision.empty()) {
          if (IsPermissionSelectorToolName(request_tool) || IsPermissionToolName(request_tool) ||
              IsPermissionRequestInput(request_arguments)) {
            suppress_body_message = true;
          }
        }

        if (!decision.empty()) {
          if (!session.agent_state_completed_requests.is_object()) {
            session.agent_state_completed_requests = nlohmann::json::object();
          }

          auto read_string_array = [](const nlohmann::json& source, std::initializer_list<const char*> keys) {
            std::vector<std::string> values;
            if (!source.is_object()) {
              return values;
            }
            for (const char* key : keys) {
              auto it = source.find(key);
              if (it == source.end() || !it->is_array()) {
                continue;
              }
              for (const auto& item : *it) {
                if (!item.is_string()) {
                  continue;
                }
                const std::string normalized_item = util::Trim(item.get<std::string>());
                if (!normalized_item.empty()) {
                  values.push_back(normalized_item);
                }
              }
              if (!values.empty()) {
                break;
              }
            }
            return values;
          };

          nlohmann::json answers = nlohmann::json::object();
          auto answers_it = normalized.find("answers");
          if (answers_it != normalized.end() && answers_it->is_object()) {
            answers = *answers_it;
          } else if (output != nullptr) {
            auto output_answers_it = output->find("answers");
            if (output_answers_it != output->end() && output_answers_it->is_object()) {
              answers = *output_answers_it;
            }
          }

          std::string mode = JsonFirstString(normalized, {"mode"}).value_or("");
          if (mode.empty() && output != nullptr) {
            mode = JsonFirstString(*output, {"mode"}).value_or("");
          }

          std::vector<std::string> allow_tools = read_string_array(normalized, {"allowTools", "allow_tools"});
          if (allow_tools.empty() && output != nullptr) {
            allow_tools = read_string_array(*output, {"allowTools", "allow_tools"});
          }

          if (decision == "approved_for_session" && allow_tools.empty()) {
            if (auto inferred = BuildPermissionToolIdentifier(request_tool, request_arguments);
                inferred.has_value()) {
              allow_tools.push_back(*inferred);
            }
          }
          if (!allow_tools.empty() && (decision == "approved" || decision == "approved_for_session")) {
            MergePermissionAllowTools(&session.permission_allow_tools, allow_tools);
          }

          std::string reason =
              JsonFirstString(normalized, {"reason", "error", "message"}).value_or("");
          if (reason.empty() && output != nullptr) {
            reason = JsonFirstString(*output, {"reason", "error", "message"}).value_or("");
            if (reason.empty()) {
              if (const nlohmann::json* output_error = JsonObjectField(*output, "error"); output_error != nullptr) {
                reason = JsonFirstString(*output_error, {"message", "error"}).value_or("");
              }
            }
          }
          if (reason.empty()) {
            if (decision == "abort") {
              reason = "Aborted by user";
            } else if (decision == "denied") {
              reason = "Denied by user";
            }
          }

          const std::int64_t now = NowMs();
          session.agent_state_requests.erase(request_it);

          nlohmann::json completed = {
              {"tool", request.value("tool", "Tool")},
              {"arguments", request_arguments},
              {"createdAt", request.value("createdAt", now)},
              {"completedAt", now},
              {"status", decision == "approved" || decision == "approved_for_session" ? "approved" : "denied"},
              {"decision", decision},
          };
          if (!reason.empty()) {
            completed["reason"] = reason;
          }
          const std::string normalized_mode = util::Trim(mode);
          if (!normalized_mode.empty() && normalized_mode != "default") {
            completed["mode"] = normalized_mode;
          }
          if (!allow_tools.empty()) {
            completed["allowTools"] = allow_tools;
          }
          if (answers.is_object() && !answers.empty()) {
            completed["answers"] = answers;
          }

          session.agent_state_completed_requests[*call_id] = std::move(completed);
          session.updated_at_ms = std::max(session.updated_at_ms, now);
          permission_state_changed = true;
          should_publish_session_update = true;
        }
      }
    }
  }

  if (!suppress_body_message) {
    nlohmann::json content = {
        {"role", "agent"},
        {"content", {
                        {"type", "codex"},
                        {"data", normalized},
                    }},
        {"meta", {{"sentFrom", "cli"}}},
    };
    bool inserted_new = false;
    const MessageRecord message =
        UpsertMessageLocked(session, std::move(content), "", /*dedupe_by_local_id=*/false, &inserted_new);
    (void)inserted_new;
    session.updated_at_ms = message.created_at_ms;

    PublishMessageReceivedLocked(session, message);
  }

  std::string resolved_title = title_change.has_value() ? util::Trim(*title_change) : "";
  const bool has_title_change = has_title_signal && !resolved_title.empty() && !session.title_initialized;
  if (has_title_change) {
    EmitAgentEventMessageLocked(session,
                                {
                                    {"type", "title-changed"},
                                    {"title", resolved_title},
                                });
    session.title_initialized = true;
  }

  if (has_title_change && session.name != resolved_title) {
    session.name = resolved_title;
    session.updated_at_ms = std::max(session.updated_at_ms, NowMs());
    should_publish_session_update = true;
  }

  if (should_publish_session_update) {
    PushEventLocked(session.ns,
                    {
                        {"type", "session-updated"},
                        {"namespace", session.ns},
                        {"sessionId", session.id},
                        {"data", BuildSessionJsonLocked(session)},
                    },
                    session.id);
  }

  if (has_title_change || permission_state_changed) {
    PersistStateLocked();
  }
  return true;
}

}  // namespace ferryman::codeagent
