#include "CodeAgentOutputParser.hpp"

#include "ferryman/util/Random.hpp"
#include "ferryman/util/StringUtil.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <regex>

namespace ferryman::codeagent::parser {

namespace {

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string MakeCodexObjectId() {
  return "codex-" + util::RandomHex(18);
}

std::string MakeCallId() {
  return "call-" + util::RandomHex(18);
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

std::optional<int> JsonFirstInt(const nlohmann::json& object, std::initializer_list<const char*> keys) {
  if (!object.is_object()) {
    return std::nullopt;
  }
  for (const char* key : keys) {
    if (key == nullptr) {
      continue;
    }
    auto it = object.find(key);
    if (it != object.end() && it->is_number_integer()) {
      return it->get<int>();
    }
    if (it != object.end() && it->is_number()) {
      return static_cast<int>(it->get<double>());
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

std::optional<std::string> ExtractCommand(const nlohmann::json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (!value.is_array()) {
    return std::nullopt;
  }

  std::string joined;
  bool first = true;
  for (const auto& part : value) {
    if (!part.is_string()) {
      continue;
    }
    if (!first) {
      joined.push_back(' ');
    }
    joined += part.get<std::string>();
    first = false;
  }
  if (joined.empty()) {
    return std::nullopt;
  }
  return joined;
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

std::optional<std::string> ExtractTextFromContent(const nlohmann::json& content) {
  if (content.is_string()) {
    return content.get<std::string>();
  }
  if (!content.is_array()) {
    return std::nullopt;
  }

  std::string joined;
  for (const auto& block : content) {
    if (!block.is_object()) {
      continue;
    }
    auto text = JsonFirstString(block, {"text", "message", "content"});
    if (!text.has_value()) {
      continue;
    }
    joined += *text;
  }
  if (joined.empty()) {
    return std::nullopt;
  }
  return joined;
}

std::optional<std::string> ExtractItemText(const nlohmann::json& item) {
  if (!item.is_object()) {
    return std::nullopt;
  }
  auto direct = JsonFirstString(item, {"text", "message"});
  if (direct.has_value()) {
    return direct;
  }
  auto content_it = item.find("content");
  if (content_it != item.end()) {
    auto from_content = ExtractTextFromContent(*content_it);
    if (from_content.has_value()) {
      return from_content;
    }
  }
  return std::nullopt;
}

std::optional<std::string> ExtractReasoningText(const nlohmann::json& item) {
  auto direct = ExtractItemText(item);
  if (direct.has_value()) {
    return direct;
  }
  if (!item.is_object()) {
    return std::nullopt;
  }

  auto it_summary = item.find("summary_text");
  if (it_summary == item.end()) {
    it_summary = item.find("summaryText");
  }
  if (it_summary == item.end()) {
    return std::nullopt;
  }
  if (!it_summary->is_array()) {
    return std::nullopt;
  }

  std::string joined;
  bool first = true;
  for (const auto& piece : *it_summary) {
    if (!piece.is_string()) {
      continue;
    }
    if (!first) {
      joined.push_back('\n');
    }
    joined += piece.get<std::string>();
    first = false;
  }
  if (joined.empty()) {
    return std::nullopt;
  }
  return joined;
}

std::string NormalizeItemType(std::string value) {
  value = ToLower(value);
  std::string normalized;
  normalized.reserve(value.size());
  for (char c : value) {
    if (c == ' ' || c == '_' || c == '-') {
      continue;
    }
    normalized.push_back(c);
  }
  return normalized;
}

std::optional<std::string> ExtractItemId(const nlohmann::json& params) {
  auto id = JsonFirstString(params, {"itemId", "item_id", "id"});
  if (id.has_value()) {
    return id;
  }
  const nlohmann::json* item = JsonObjectField(params, "item");
  if (item == nullptr) {
    return std::nullopt;
  }
  return JsonFirstString(*item, {"id", "itemId", "item_id"});
}

nlohmann::json ExtractChanges(const nlohmann::json& value) {
  if (value.is_object()) {
    return value;
  }
  if (!value.is_array()) {
    return nlohmann::json::object();
  }

  nlohmann::json changes = nlohmann::json::object();
  for (const auto& entry : value) {
    if (!entry.is_object()) {
      continue;
    }
    auto path = JsonFirstString(entry, {"path", "file", "filePath", "file_path"});
    if (!path.has_value()) {
      continue;
    }
    changes[*path] = entry;
  }
  return changes;
}

nlohmann::json CodexReasoningBody(const std::string& text) {
  return {
      {"type", "reasoning"},
      {"message", text},
      {"id", MakeCodexObjectId()},
  };
}

nlohmann::json CodexToolCallBody(const std::string& name, const std::string& call_id, const nlohmann::json& input) {
  return {
      {"type", "tool-call"},
      {"name", name},
      {"callId", call_id},
      {"input", input},
      {"id", MakeCodexObjectId()},
  };
}

nlohmann::json CodexToolResultBody(const std::string& call_id, const nlohmann::json& output, bool is_error = false) {
  return {
      {"type", "tool-call-result"},
      {"callId", call_id},
      {"output", output},
      {"is_error", is_error},
      {"id", MakeCodexObjectId()},
  };
}

nlohmann::json CodexTokenCountBody(const nlohmann::json& info) {
  return {
      {"type", "token_count"},
      {"info", info.is_object() ? info : nlohmann::json::object()},
      {"id", MakeCodexObjectId()},
  };
}

void MergeTokenInfoFromObject(const nlohmann::json& source, nlohmann::json* target) {
  if (!source.is_object() || target == nullptr || !target->is_object()) {
    return;
  }
  static constexpr std::array<std::string_view, 18> kTokenKeys = {
      "input_tokens", "output_tokens", "cache_creation_input_tokens", "cache_read_input_tokens",
      "cached_input_tokens", "total_tokens", "prompt_tokens", "completion_tokens", "inputTokens", "outputTokens",
      "cachedInputTokens", "totalTokens", "promptTokens", "completionTokens", "reasoning_output_tokens",
      "reasoningOutputTokens", "model_context_window", "modelContextWindow",
  };
  for (std::string_view key : kTokenKeys) {
    const std::string key_str(key);
    auto it = source.find(key_str);
    if (it != source.end() && !it->is_null()) {
      (*target)[key_str] = *it;
    }
  }
}

void MergeTokenInfoFromObjectIfMissing(const nlohmann::json& source, nlohmann::json* target) {
  if (!source.is_object() || target == nullptr || !target->is_object()) {
    return;
  }
  static constexpr std::array<std::string_view, 18> kTokenKeys = {
      "input_tokens", "output_tokens", "cache_creation_input_tokens", "cache_read_input_tokens",
      "cached_input_tokens", "total_tokens", "prompt_tokens", "completion_tokens", "inputTokens", "outputTokens",
      "cachedInputTokens", "totalTokens", "promptTokens", "completionTokens", "reasoning_output_tokens",
      "reasoningOutputTokens", "model_context_window", "modelContextWindow",
  };
  for (std::string_view key : kTokenKeys) {
    const std::string key_str(key);
    if (target->contains(key_str)) {
      continue;
    }
    auto it = source.find(key_str);
    if (it != source.end() && !it->is_null()) {
      (*target)[key_str] = *it;
    }
  }
}

void MergeTokenInfoFromUsageEnvelope(const nlohmann::json& usage, nlohmann::json* target) {
  if (!usage.is_object() || target == nullptr || !target->is_object()) {
    return;
  }

  const nlohmann::json* last = JsonObjectField(usage, "last");
  if (last == nullptr) {
    last = JsonObjectField(usage, "last_token_usage");
  }
  if (last == nullptr) {
    last = JsonObjectField(usage, "lastTokenUsage");
  }
  if (last != nullptr) {
    MergeTokenInfoFromObject(*last, target);
  }

  const nlohmann::json* total = JsonObjectField(usage, "total");
  if (total == nullptr) {
    total = JsonObjectField(usage, "total_token_usage");
  }
  if (total == nullptr) {
    total = JsonObjectField(usage, "totalTokenUsage");
  }
  if (total != nullptr) {
    auto total_input = JsonFirstInt(*total, {"input_tokens", "inputTokens", "prompt_tokens", "promptTokens"});
    auto total_output = JsonFirstInt(*total, {"output_tokens", "outputTokens", "completion_tokens", "completionTokens"});
    auto total_cache_creation =
        JsonFirstInt(*total, {"cache_creation_input_tokens", "cacheCreationInputTokens"});
    auto total_cache_read = JsonFirstInt(*total, {"cache_read_input_tokens", "cacheReadInputTokens"});
    auto total_cached_input = JsonFirstInt(*total, {"cached_input_tokens", "cachedInputTokens"});
    auto total_reasoning_output =
        JsonFirstInt(*total, {"reasoning_output_tokens", "reasoningOutputTokens"});

    if (total_input.has_value()) {
      (*target)["input_tokens"] = *total_input;
    }
    if (total_output.has_value()) {
      (*target)["output_tokens"] = *total_output;
    }
    if (total_cache_creation.has_value()) {
      (*target)["cache_creation_input_tokens"] = *total_cache_creation;
    }
    if (total_cache_read.has_value()) {
      (*target)["cache_read_input_tokens"] = *total_cache_read;
    }
    if (total_cached_input.has_value()) {
      (*target)["cached_input_tokens"] = *total_cached_input;
    }
    if (total_reasoning_output.has_value()) {
      (*target)["reasoning_output_tokens"] = *total_reasoning_output;
    }

    MergeTokenInfoFromObjectIfMissing(*total, target);
    if (auto total_tokens = JsonFirstInt(*total, {"total_tokens", "totalTokens"}); total_tokens.has_value()) {
      (*target)["total_tokens"] = *total_tokens;
    } else {
      if (total_input.has_value() && total_output.has_value()) {
        (*target)["total_tokens"] = std::max(0, *total_input + *total_output);
      }
    }
    if (auto total_context_window =
            JsonFirstInt(*total, {"model_context_window", "modelContextWindow"});
        total_context_window.has_value() && !target->contains("model_context_window")) {
      (*target)["model_context_window"] = *total_context_window;
    }
  }

  MergeTokenInfoFromObject(usage, target);
  if (auto context_window = JsonFirstInt(usage, {"model_context_window", "modelContextWindow"});
      context_window.has_value()) {
    (*target)["model_context_window"] = *context_window;
  }
}

nlohmann::json ExtractTokenUsageInfo(const nlohmann::json& params) {
  if (!params.is_object()) {
    return nlohmann::json::object();
  }

  if (const auto* usage = JsonObjectField(params, "usage")) {
    nlohmann::json info = nlohmann::json::object();
    MergeTokenInfoFromUsageEnvelope(*usage, &info);
    return info.empty() ? *usage : info;
  }
  if (const auto* usage = JsonObjectField(params, "tokenUsage")) {
    nlohmann::json info = nlohmann::json::object();
    MergeTokenInfoFromUsageEnvelope(*usage, &info);
    return info.empty() ? *usage : info;
  }
  if (const auto* usage = JsonObjectField(params, "token_usage")) {
    nlohmann::json info = nlohmann::json::object();
    MergeTokenInfoFromUsageEnvelope(*usage, &info);
    return info.empty() ? *usage : info;
  }

  if (const auto* turn = JsonObjectField(params, "turn")) {
    if (const auto* usage = JsonObjectField(*turn, "usage")) {
      nlohmann::json info = nlohmann::json::object();
      MergeTokenInfoFromUsageEnvelope(*usage, &info);
      return info.empty() ? *usage : info;
    }
    if (const auto* usage = JsonObjectField(*turn, "tokenUsage")) {
      nlohmann::json info = nlohmann::json::object();
      MergeTokenInfoFromUsageEnvelope(*usage, &info);
      return info.empty() ? *usage : info;
    }
    if (const auto* usage = JsonObjectField(*turn, "token_usage")) {
      nlohmann::json info = nlohmann::json::object();
      MergeTokenInfoFromUsageEnvelope(*usage, &info);
      return info.empty() ? *usage : info;
    }
  }

  nlohmann::json info = nlohmann::json::object();
  MergeTokenInfoFromUsageEnvelope(params, &info);
  MergeTokenInfoFromObject(params, &info);
  if (const auto* turn = JsonObjectField(params, "turn")) {
    MergeTokenInfoFromUsageEnvelope(*turn, &info);
    MergeTokenInfoFromObject(*turn, &info);
  }
  return info;
}

bool ParseReasoningTitleContent(const std::string& text, std::string* title, std::string* content) {
  const std::string trimmed = util::Trim(text);
  if (trimmed.size() < 4 || trimmed.rfind("**", 0) != 0) {
    return false;
  }
  const size_t end = trimmed.find("**", 2);
  if (end == std::string::npos || end <= 2) {
    return false;
  }
  if (title != nullptr) {
    *title = trimmed.substr(2, end - 2);
  }
  if (content != nullptr) {
    *content = util::Trim(trimmed.substr(end + 2));
  }
  return true;
}

std::optional<std::string> TryParseCursorAssistantText(const nlohmann::json& event) {
  const nlohmann::json* message = JsonObjectField(event, "message");
  if (message == nullptr) {
    return std::nullopt;
  }
  auto content_it = message->find("content");
  if (content_it == message->end() || !content_it->is_array()) {
    return std::nullopt;
  }

  std::string text;
  for (const auto& part : *content_it) {
    if (!part.is_object()) {
      continue;
    }
    auto type = JsonFirstString(part, {"type"});
    auto chunk = JsonFirstString(part, {"text"});
    if (type.has_value() && *type == "text" && chunk.has_value()) {
      text += *chunk;
    }
  }
  if (text.empty()) {
    return std::nullopt;
  }
  return text;
}

std::string ExtractCursorToolName(const nlohmann::json& tool_call) {
  if (!tool_call.is_object()) {
    return "unknown";
  }
  if (tool_call.contains("readToolCall")) {
    return "read_file";
  }
  if (tool_call.contains("writeToolCall")) {
    return "write_file";
  }
  if (const auto* fn = JsonObjectField(tool_call, "function")) {
    auto name = JsonFirstString(*fn, {"name"});
    if (name.has_value()) {
      return *name;
    }
  }
  return "unknown";
}

nlohmann::json ExtractCursorToolInput(const nlohmann::json& tool_call) {
  if (!tool_call.is_object()) {
    return nlohmann::json::object();
  }
  if (const auto* read = JsonObjectField(tool_call, "readToolCall")) {
    auto it = read->find("args");
    return it == read->end() ? nlohmann::json::object() : *it;
  }
  if (const auto* write = JsonObjectField(tool_call, "writeToolCall")) {
    auto it = write->find("args");
    return it == write->end() ? nlohmann::json::object() : *it;
  }
  if (const auto* fn = JsonObjectField(tool_call, "function")) {
    nlohmann::json args = nullptr;
    auto it = fn->find("arguments");
    if (it != fn->end()) {
      args = *it;
    }
    return nlohmann::json{{"arguments", args}};
  }
  return nlohmann::json::object();
}

nlohmann::json ExtractCursorToolResult(const nlohmann::json& tool_call) {
  if (!tool_call.is_object()) {
    return nlohmann::json::object();
  }
  if (const auto* read = JsonObjectField(tool_call, "readToolCall")) {
    auto it_result = read->find("result");
    return it_result == read->end() ? *read : *it_result;
  }
  if (const auto* write = JsonObjectField(tool_call, "writeToolCall")) {
    auto it_result = write->find("result");
    return it_result == write->end() ? *write : *it_result;
  }
  return nlohmann::json::object();
}

std::optional<std::string> BuildMcpToolName(const nlohmann::json& payload) {
  if (!payload.is_object()) {
    return std::nullopt;
  }
  const nlohmann::json* invocation = JsonObjectField(payload, "invocation");
  std::optional<std::string> server;
  std::optional<std::string> tool;

  if (invocation != nullptr) {
    server = JsonFirstString(*invocation, {"server", "server_name"});
    tool = JsonFirstString(*invocation, {"tool", "tool_name"});
  }
  if (!server.has_value()) {
    server = JsonFirstString(payload, {"server", "server_name"});
  }
  if (!tool.has_value()) {
    tool = JsonFirstString(payload, {"tool", "tool_name"});
  }

  if (!server.has_value() || !tool.has_value()) {
    return std::nullopt;
  }
  return "mcp__" + *server + "__" + *tool;
}

bool HandleNotificationJson(const std::string& method, const nlohmann::json& params, AgentOutputParseState* state,
                            std::vector<nlohmann::json>* out_bodies) {
  if (method.rfind("codex/event/", 0) == 0) {
    const nlohmann::json* msg = JsonObjectField(params, "msg");
    if (msg == nullptr) {
      return true;
    }
    auto msg_type = JsonFirstString(*msg, {"type"});
    if (!msg_type.has_value()) {
      return true;
    }

    if (*msg_type == "item_started" || *msg_type == "item_completed") {
      const char* item_method = *msg_type == "item_started" ? "item/started" : "item/completed";
      nlohmann::json nested_params = {
          {"item", msg->value("item", nlohmann::json::object())},
          {"itemId", JsonFirstString(*msg, {"item_id", "itemId"}).value_or("")},
      };
      return HandleNotificationJson(item_method, nested_params, state, out_bodies);
    }

    if (*msg_type == "agent_message_delta" || *msg_type == "agent_message_content_delta") {
      nlohmann::json nested_params = {
          {"itemId", JsonFirstString(*msg, {"item_id", "itemId", "id"}).value_or("agent-message")},
          {"delta", JsonFirstString(*msg, {"delta", "text", "message"}).value_or("")},
      };
      return HandleNotificationJson("item/agentMessage/delta", nested_params, state, out_bodies);
    }

    if (*msg_type == "reasoning_content_delta") {
      nlohmann::json nested_params = {
          {"itemId", JsonFirstString(*msg, {"item_id", "itemId", "id"}).value_or("reasoning")},
          {"delta", JsonFirstString(*msg, {"delta", "text", "message"}).value_or("")},
      };
      return HandleNotificationJson("item/reasoning/summaryTextDelta", nested_params, state, out_bodies);
    }

    if (*msg_type == "exec_command_output_delta") {
      nlohmann::json nested_params = {
          {"itemId", JsonFirstString(*msg, {"call_id", "callId", "item_id", "itemId", "id"}).value_or("")},
          {"delta", JsonFirstString(*msg, {"delta", "output", "stdout", "text"}).value_or("")},
      };
      return HandleNotificationJson("item/commandExecution/outputDelta", nested_params, state, out_bodies);
    }

    return ParseAgentOutputJson(*msg, state, out_bodies);
  }

  if (method == "item/agentMessage/delta") {
    auto item_id = ExtractItemId(params);
    auto delta = JsonFirstString(params, {"delta", "text", "message"});
    if (item_id.has_value() && delta.has_value()) {
      state->agent_message_buffers[*item_id] += *delta;
    }
    return true;
  }

  if (method == "item/reasoning/textDelta" || method == "item/reasoning/summaryTextDelta") {
    auto item_id = ExtractItemId(params);
    auto delta = JsonFirstString(params, {"delta", "text", "message"});
    if (!item_id.has_value()) {
      item_id = std::string("reasoning");
    }
    if (delta.has_value()) {
      state->reasoning_buffers[*item_id] += *delta;
      state->live_reasoning_buffer += *delta;
    }
    return true;
  }

  if (method == "item/commandExecution/requestApproval") {
    const std::string call_id = ExtractItemId(params).value_or(MakeCallId());
    nlohmann::json input = nlohmann::json::object();

    auto command_it = params.find("command");
    if (command_it != params.end()) {
      auto command = ExtractCommand(*command_it);
      if (command.has_value()) {
        input["command"] = *command;
      } else {
        input["command"] = *command_it;
      }
    } else {
      auto args_it = params.find("args");
      if (args_it != params.end()) {
        auto command = ExtractCommand(*args_it);
        if (command.has_value()) {
          input["command"] = *command;
        } else {
          input["command"] = *args_it;
        }
      }
    }

    auto reason = JsonFirstString(params, {"reason", "message"});
    if (reason.has_value()) {
      input["message"] = *reason;
    }
    auto cwd = JsonFirstString(params, {"cwd", "workingDirectory", "working_directory"});
    if (cwd.has_value()) {
      input["cwd"] = *cwd;
    }
    input["approval_request"] = true;

    out_bodies->push_back(CodexToolCallBody("CodexBash", call_id, input));
    return true;
  }

  if (method == "item/fileChange/requestApproval") {
    const std::string call_id = ExtractItemId(params).value_or(MakeCallId());
    nlohmann::json input = nlohmann::json::object();

    auto reason = JsonFirstString(params, {"reason", "message"});
    if (reason.has_value()) {
      input["message"] = *reason;
    }
    auto grant_root = JsonFirstString(params, {"grantRoot", "grant_root"});
    if (grant_root.has_value()) {
      input["grantRoot"] = *grant_root;
    }

    auto changes_it = params.find("changes");
    if (changes_it != params.end()) {
      input["changes"] = ExtractChanges(*changes_it);
    } else {
      auto change_it = params.find("change");
      if (change_it != params.end()) {
        input["changes"] = ExtractChanges(*change_it);
      } else {
        auto diff_it = params.find("diff");
        if (diff_it != params.end()) {
          input["changes"] = ExtractChanges(*diff_it);
        }
      }
    }

    input["approval_request"] = true;

    out_bodies->push_back(CodexToolCallBody("CodexPatch", call_id, input));
    return true;
  }

  if (method == "item/tool/requestUserInput") {
    const std::string call_id = ExtractItemId(params).value_or(MakeCallId());
    nlohmann::json input = params;
    input.erase("itemId");
    input.erase("item_id");
    input.erase("id");
    input["approval_request"] = true;
    out_bodies->push_back(CodexToolCallBody("request_user_input", call_id, input));
    return true;
  }

  if (method == "item/commandExecution/outputDelta") {
    auto item_id = ExtractItemId(params);
    auto delta = JsonFirstString(params, {"delta", "text", "output", "stdout"});
    if (item_id.has_value() && delta.has_value()) {
      state->command_output_buffers[*item_id] += *delta;
    }
    return true;
  }

  if (method == "turn/diff/updated") {
    nlohmann::json event = {
        {"type", "turn_diff"},
        {"unified_diff", JsonFirstString(params, {"diff", "unified_diff", "unifiedDiff"}).value_or("")},
    };
    return ParseAgentOutputJson(event, state, out_bodies);
  }

  if (method == "turn/completed") {
    const nlohmann::json token_info = ExtractTokenUsageInfo(params);
    if (token_info.is_object() && !token_info.empty()) {
      out_bodies->push_back(CodexTokenCountBody(token_info));
    }

    std::string status = ToLower(JsonFirstString(params, {"status"}).value_or(""));
    const auto* turn = JsonObjectField(params, "turn");
    if (turn != nullptr) {
      auto turn_status = JsonFirstString(*turn, {"status"});
      if (status.empty() && turn_status.has_value()) {
        status = ToLower(*turn_status);
      }
    }
    if (status == "failed" || status == "error") {
      nlohmann::json event = {
          {"type", "task_failed"},
          {"error", JsonFirstString(params, {"error", "message", "reason"}).value_or("task failed")},
      };
      return ParseAgentOutputJson(event, state, out_bodies);
    }
    if (status == "interrupted" || status == "cancelled" || status == "canceled") {
      return ParseAgentOutputJson(nlohmann::json{{"type", "turn_aborted"}}, state, out_bodies);
    }
    return ParseAgentOutputJson(nlohmann::json{{"type", "task_complete"}}, state, out_bodies);
  }

  if (method == "error") {
    auto will_retry = JsonFirstBool(params, {"will_retry", "willRetry"});
    if (will_retry.value_or(false)) {
      return true;
    }
    nlohmann::json event = {
        {"type", "task_failed"},
        {"error", JsonFirstString(params, {"message"}).value_or("task failed")},
    };
    return ParseAgentOutputJson(event, state, out_bodies);
  }

  if (method == "item/started" || method == "item/completed") {
    const bool completed = method == "item/completed";
    const nlohmann::json* item_ptr = JsonObjectField(params, "item");
    const nlohmann::json item = item_ptr == nullptr ? params : *item_ptr;

    const auto item_id = ExtractItemId(params);
    if (!item_id.has_value()) {
      return true;
    }
    const auto raw_type = JsonFirstString(item, {"type", "itemType", "kind"}).value_or("");
    const std::string item_type = NormalizeItemType(raw_type);

    if (item_type == "agentmessage") {
      if (!completed) {
        return true;
      }
      auto text = ExtractItemText(item);
      if (!text.has_value()) {
        auto it = state->agent_message_buffers.find(*item_id);
        if (it != state->agent_message_buffers.end()) {
          text = it->second;
        }
      }
      if (text.has_value() && !text->empty()) {
        out_bodies->push_back(CodexMessageBody(*text));
      }
      state->agent_message_buffers.erase(*item_id);
      return true;
    }

    if (item_type == "reasoning") {
      if (!completed) {
        return true;
      }
      auto text = ExtractReasoningText(item);
      if (!text.has_value()) {
        auto it = state->reasoning_buffers.find(*item_id);
        if (it != state->reasoning_buffers.end()) {
          text = it->second;
        }
      }
      if (text.has_value() && !text->empty()) {
        out_bodies->push_back(CodexReasoningBody(*text));
      }
      state->reasoning_buffers.erase(*item_id);
      state->live_reasoning_buffer.clear();
      return true;
    }

    if (item_type == "commandexecution") {
      nlohmann::json event = {
          {"type", completed ? "exec_command_end" : "exec_command_begin"},
          {"call_id", *item_id},
      };
      if (!completed) {
        auto command_it = item.find("command");
        if (command_it != item.end()) {
          event["command"] = *command_it;
        } else {
          auto args_it = item.find("args");
          if (args_it != item.end()) {
            event["command"] = *args_it;
          }
        }
        auto cwd = JsonFirstString(item, {"cwd", "workingDirectory", "working_directory"});
        if (cwd.has_value()) {
          event["cwd"] = *cwd;
        }
        auto auto_approved = JsonFirstBool(item, {"autoApproved", "auto_approved"});
        if (auto_approved.has_value()) {
          event["auto_approved"] = *auto_approved;
        }
      } else {
        auto output = JsonFirstString(item, {"output", "result", "stdout", "aggregated_output", "aggregatedOutput"});
        if (!output.has_value()) {
          auto it = state->command_output_buffers.find(*item_id);
          if (it != state->command_output_buffers.end()) {
            output = it->second;
          }
        }
        if (output.has_value()) {
          event["output"] = *output;
        }
        auto stderr_text = JsonFirstString(item, {"stderr"});
        if (stderr_text.has_value()) {
          event["stderr"] = *stderr_text;
        }
        auto error = JsonFirstString(item, {"error"});
        if (error.has_value()) {
          event["error"] = *error;
        }
        auto exit_code = JsonFirstInt(item, {"exitCode", "exit_code", "exitcode"});
        if (exit_code.has_value()) {
          event["exit_code"] = *exit_code;
        }
      }
      state->command_output_buffers.erase(*item_id);
      return ParseAgentOutputJson(event, state, out_bodies);
    }

    if (item_type == "filechange") {
      nlohmann::json event = {
          {"type", completed ? "patch_apply_end" : "patch_apply_begin"},
          {"call_id", *item_id},
      };
      if (!completed) {
        auto changes_it = item.find("changes");
        if (changes_it != item.end()) {
          event["changes"] = ExtractChanges(*changes_it);
        } else {
          auto change_it = item.find("change");
          if (change_it != item.end()) {
            event["changes"] = ExtractChanges(*change_it);
          }
        }
        auto auto_approved = JsonFirstBool(item, {"autoApproved", "auto_approved"});
        if (auto_approved.has_value()) {
          event["auto_approved"] = *auto_approved;
        }
      } else {
        auto stdout_text = JsonFirstString(item, {"stdout", "output"});
        auto stderr_text = JsonFirstString(item, {"stderr"});
        auto success = JsonFirstBool(item, {"success", "ok", "applied"});
        if (stdout_text.has_value()) {
          event["stdout"] = *stdout_text;
        }
        if (stderr_text.has_value()) {
          event["stderr"] = *stderr_text;
        }
        event["success"] = success.value_or(false);
      }
      return ParseAgentOutputJson(event, state, out_bodies);
    }

    if (item_type == "mcptoolcall") {
      if (!completed) {
        auto tool_name = BuildMcpToolName(item);
        if (!tool_name.has_value()) {
          return true;
        }
        const nlohmann::json* invocation = JsonObjectField(item, "invocation");
        nlohmann::json input = nlohmann::json::object();
        if (invocation != nullptr) {
          auto args_it = invocation->find("arguments");
          if (args_it != invocation->end()) {
            input = *args_it;
          } else {
            auto input_it = invocation->find("input");
            if (input_it != invocation->end()) {
              input = *input_it;
            }
          }
        } else {
          auto args_it = item.find("arguments");
          if (args_it != item.end()) {
            input = *args_it;
          } else {
            auto input_it = item.find("input");
            if (input_it != item.end()) {
              input = *input_it;
            }
          }
        }
        out_bodies->push_back(CodexToolCallBody(*tool_name, *item_id, input));
      } else {
        bool is_error = false;
        nlohmann::json output = item.value("result", nlohmann::json::object());
        if (output.is_object()) {
          auto ok_it = output.find("Ok");
          auto err_it = output.find("Err");
          if (ok_it != output.end()) {
            output = *ok_it;
          } else if (err_it != output.end()) {
            output = *err_it;
            is_error = true;
          }
        }
        out_bodies->push_back(CodexToolResultBody(*item_id, output, is_error));
      }
      return true;
    }

    return true;
  }

  if (method == "thread/tokenUsage/updated") {
    const nlohmann::json token_info = ExtractTokenUsageInfo(params);
    if (token_info.is_object() && !token_info.empty()) {
      out_bodies->push_back(CodexTokenCountBody(token_info));
    }
    return true;
  }

  if (method == "thread/started" || method == "thread/resumed" || method == "turn/started" ||
      method == "account/rateLimits/updated" || method == "turn/plan/updated" || method == "thread/compacted") {
    return true;
  }

  return false;
}

}  // namespace

nlohmann::json CodexMessageBody(const std::string& text) {
  return {
      {"type", "message"},
      {"message", text},
      {"id", MakeCodexObjectId()},
  };
}

bool ParseAgentOutputJson(const nlohmann::json& payload, AgentOutputParseState* state,
                          std::vector<nlohmann::json>* out_bodies) {
  if (!payload.is_object() || state == nullptr || out_bodies == nullptr) {
    return false;
  }

  auto method = JsonFirstString(payload, {"method"});
  if (method.has_value()) {
    const nlohmann::json params = payload.value("params", nlohmann::json::object());
    return HandleNotificationJson(*method, params, state, out_bodies);
  }

  auto type_opt = JsonFirstString(payload, {"type"});
  if (!type_opt.has_value()) {
    return false;
  }
  const std::string type = *type_opt;

  if (type.find('.') != std::string::npos) {
    std::string mapped_method = type;
    std::replace(mapped_method.begin(), mapped_method.end(), '.', '/');
    nlohmann::json params = payload;
    params.erase("type");
    if (HandleNotificationJson(mapped_method, params, state, out_bodies)) {
      return true;
    }
    return true;
  }

  if (type == "message") {
    auto message = JsonFirstString(payload, {"message"});
    if (!message.has_value()) {
      return false;
    }
    nlohmann::json body = payload;
    if (!JsonFirstString(body, {"id"}).has_value()) {
      body["id"] = MakeCodexObjectId();
    }
    out_bodies->push_back(std::move(body));
    return true;
  }

  if (type == "reasoning") {
    auto message = JsonFirstString(payload, {"message"});
    if (!message.has_value()) {
      return false;
    }
    nlohmann::json body = payload;
    if (!JsonFirstString(body, {"id"}).has_value()) {
      body["id"] = MakeCodexObjectId();
    }
    out_bodies->push_back(std::move(body));
    return true;
  }

  if (type == "tool-call") {
    nlohmann::json body = payload;
    auto call_id = JsonFirstString(body, {"callId"});
    if (!call_id.has_value()) {
      call_id = ExtractCallId(body);
      if (call_id.has_value()) {
        body["callId"] = *call_id;
      }
    }
    if (!call_id.has_value()) {
      return false;
    }
    if (!JsonFirstString(body, {"name"}).has_value()) {
      body["name"] = "Tool";
    }
    if (!body.contains("input")) {
      body["input"] = nlohmann::json::object();
    }
    if (!JsonFirstString(body, {"id"}).has_value()) {
      body["id"] = MakeCodexObjectId();
    }
    out_bodies->push_back(std::move(body));
    return true;
  }

  if (type == "tool-call-result") {
    nlohmann::json body = payload;
    auto call_id = JsonFirstString(body, {"callId"});
    if (!call_id.has_value()) {
      call_id = ExtractCallId(body);
      if (call_id.has_value()) {
        body["callId"] = *call_id;
      }
    }
    if (!call_id.has_value()) {
      return false;
    }
    if (!body.contains("output")) {
      body["output"] = nlohmann::json::object();
    }
    if (!JsonFirstString(body, {"id"}).has_value()) {
      body["id"] = MakeCodexObjectId();
    }
    out_bodies->push_back(std::move(body));
    return true;
  }

  if (type == "assistant") {
    const nlohmann::json* message = JsonObjectField(payload, "message");
    nlohmann::json token_info = nlohmann::json::object();
    if (message != nullptr) {
      token_info = ExtractTokenUsageInfo(*message);
    }
    if ((token_info.is_discarded() || token_info.empty()) && payload.is_object()) {
      token_info = ExtractTokenUsageInfo(payload);
    }
    if (token_info.is_object() && !token_info.empty()) {
      out_bodies->push_back(CodexTokenCountBody(token_info));
    }

    bool emitted = false;
    if (message != nullptr) {
      auto content_it = message->find("content");
      if (content_it != message->end() && content_it->is_array()) {
        for (const auto& block : *content_it) {
          if (!block.is_object()) {
            continue;
          }
          const std::string block_type = JsonFirstString(block, {"type"}).value_or("");
          if (block_type == "text") {
            auto text = JsonFirstString(block, {"text"});
            if (text.has_value() && !text->empty()) {
              out_bodies->push_back(CodexMessageBody(*text));
              emitted = true;
            }
            continue;
          }
          if (block_type == "thinking") {
            auto thinking = JsonFirstString(block, {"thinking", "text"});
            if (thinking.has_value() && !thinking->empty()) {
              out_bodies->push_back(CodexReasoningBody(*thinking));
              emitted = true;
            }
            continue;
          }
          if (block_type == "tool_use") {
            const std::string call_id = JsonFirstString(block, {"id"}).value_or(MakeCallId());
            const std::string name = JsonFirstString(block, {"name"}).value_or("Tool");
            nlohmann::json input = nlohmann::json::object();
            auto input_it = block.find("input");
            if (input_it != block.end()) {
              input = *input_it;
            }
            out_bodies->push_back(CodexToolCallBody(name, call_id, input));
            emitted = true;
          }
        }
      }
    }
    if (!emitted) {
      auto text = TryParseCursorAssistantText(payload);
      if (text.has_value() && !text->empty()) {
        out_bodies->push_back(CodexMessageBody(*text));
      }
    }
    return true;
  }

  if (type == "user") {
    const nlohmann::json* message = JsonObjectField(payload, "message");
    if (message == nullptr) {
      return true;
    }
    auto content_it = message->find("content");
    if (content_it == message->end() || !content_it->is_array()) {
      return true;
    }

    for (const auto& block : *content_it) {
      if (!block.is_object()) {
        continue;
      }
      const std::string block_type = JsonFirstString(block, {"type"}).value_or("");
      if (block_type != "tool_result") {
        continue;
      }
      auto call_id = JsonFirstString(block, {"tool_use_id", "toolUseId", "id"});
      if (!call_id.has_value() || call_id->empty()) {
        continue;
      }
      nlohmann::json output = block;
      output.erase("type");
      output.erase("tool_use_id");
      output.erase("toolUseId");
      const bool is_error = JsonFirstBool(block, {"is_error", "isError"}).value_or(false);
      out_bodies->push_back(CodexToolResultBody(*call_id, output, is_error));
    }
    return true;
  }

  if (type == "system" || type == "log") {
    return true;
  }
  if (type == "thinking") {
    auto subtype = JsonFirstString(payload, {"subtype"}).value_or("");
    auto text = JsonFirstString(payload, {"text"});
    if (subtype == "completed" && text.has_value() && !text->empty()) {
      out_bodies->push_back(CodexReasoningBody(*text));
    }
    return true;
  }
  if (type == "tool_call") {
    const std::string call_id = JsonFirstString(payload, {"call_id", "callId"}).value_or(MakeCallId());
    const std::string subtype = JsonFirstString(payload, {"subtype"}).value_or("");
    const nlohmann::json* tool_call = JsonObjectField(payload, "tool_call");
    const nlohmann::json empty_object = nlohmann::json::object();
    const nlohmann::json& tool = tool_call == nullptr ? empty_object : *tool_call;

    if (subtype == "started") {
      out_bodies->push_back(CodexToolCallBody(ExtractCursorToolName(tool), call_id, ExtractCursorToolInput(tool)));
    } else if (subtype == "completed") {
      out_bodies->push_back(CodexToolResultBody(call_id, ExtractCursorToolResult(tool)));
    }
    return true;
  }
  if (type == "result") {
    const nlohmann::json token_info = ExtractTokenUsageInfo(payload);
    if (token_info.is_object() && !token_info.empty()) {
      out_bodies->push_back(CodexTokenCountBody(token_info));
    }
    return true;
  }

  if (type == "event_msg") {
    const nlohmann::json* raw = JsonObjectField(payload, "payload");
    if (raw != nullptr) {
      return ParseAgentOutputJson(*raw, state, out_bodies);
    }
    return true;
  }

  if (type == "response_item") {
    const nlohmann::json* raw = JsonObjectField(payload, "payload");
    if (raw == nullptr) {
      return true;
    }
    const std::string item_type = JsonFirstString(*raw, {"type"}).value_or("");
    if (item_type == "function_call") {
      auto name = JsonFirstString(*raw, {"name"});
      auto call_id = ExtractCallId(*raw);
      if (!name.has_value() || !call_id.has_value()) {
        return true;
      }
      nlohmann::json input = nlohmann::json::object();
      auto it_args = raw->find("arguments");
      if (it_args != raw->end()) {
        input = ParseMaybeJsonString(*it_args);
      }
      out_bodies->push_back(CodexToolCallBody(*name, *call_id, input));
      return true;
    }
    if (item_type == "function_call_output") {
      auto call_id = ExtractCallId(*raw);
      if (!call_id.has_value()) {
        return true;
      }
      auto it_output = raw->find("output");
      const nlohmann::json output = it_output == raw->end() ? nlohmann::json::object() : *it_output;
      out_bodies->push_back(CodexToolResultBody(*call_id, output));
      return true;
    }
    return true;
  }

  if (type == "agent_message") {
    auto text = JsonFirstString(payload, {"message", "text"});
    if (text.has_value() && !text->empty()) {
      out_bodies->push_back(CodexMessageBody(*text));
    }
    return true;
  }

  if (type == "agent_reasoning_delta") {
    auto delta = JsonFirstString(payload, {"delta", "text", "message"});
    if (delta.has_value()) {
      state->live_reasoning_buffer += *delta;
      if (!state->reasoning_tool_started) {
        std::string title;
        std::string content;
        if (ParseReasoningTitleContent(state->live_reasoning_buffer, &title, &content) && !title.empty()) {
          state->reasoning_tool_started = true;
          state->reasoning_tool_call_id = MakeCallId();
          state->reasoning_title = title;
          out_bodies->push_back(CodexToolCallBody("CodexReasoning", state->reasoning_tool_call_id,
                                                  nlohmann::json{{"title", title}}));
        }
      }
    }
    return true;
  }

  if (type == "agent_reasoning_section_break") {
    if (state->reasoning_tool_started) {
      std::string title;
      std::string content;
      if (!ParseReasoningTitleContent(state->live_reasoning_buffer, &title, &content)) {
        content = util::Trim(state->live_reasoning_buffer);
      }
      out_bodies->push_back(CodexToolResultBody(
          state->reasoning_tool_call_id, nlohmann::json{{"content", content}, {"status", "canceled"}}));
    } else if (!state->live_reasoning_buffer.empty()) {
      out_bodies->push_back(CodexReasoningBody(state->live_reasoning_buffer));
    }
    state->reasoning_tool_started = false;
    state->reasoning_tool_call_id.clear();
    state->reasoning_title.clear();
    state->live_reasoning_buffer.clear();
    return true;
  }

  if (type == "agent_reasoning") {
    std::string final_text = JsonFirstString(payload, {"text", "message"}).value_or(state->live_reasoning_buffer);
    if (!final_text.empty()) {
      std::string title;
      std::string content;
      if (ParseReasoningTitleContent(final_text, &title, &content) && !title.empty()) {
        if (!state->reasoning_tool_started) {
          state->reasoning_tool_started = true;
          state->reasoning_tool_call_id = MakeCallId();
          state->reasoning_title = title;
          out_bodies->push_back(CodexToolCallBody("CodexReasoning", state->reasoning_tool_call_id,
                                                  nlohmann::json{{"title", title}}));
        }
        out_bodies->push_back(CodexToolResultBody(
            state->reasoning_tool_call_id, nlohmann::json{{"content", content}, {"status", "completed"}}));
      } else if (state->reasoning_tool_started) {
        out_bodies->push_back(CodexToolResultBody(
            state->reasoning_tool_call_id,
            nlohmann::json{{"content", util::Trim(final_text)}, {"status", "completed"}}));
      } else {
        out_bodies->push_back(CodexReasoningBody(final_text));
      }
    }
    state->reasoning_tool_started = false;
    state->reasoning_tool_call_id.clear();
    state->reasoning_title.clear();
    state->live_reasoning_buffer.clear();
    return true;
  }

  if (type == "exec_command_begin" || type == "exec_approval_request") {
    const std::string call_id = ExtractCallId(payload).value_or(MakeCallId());
    nlohmann::json input = payload;
    input.erase("type");
    input.erase("call_id");
    input.erase("callId");
    if (type == "exec_approval_request") {
      input["approval_request"] = true;
    }

    state->command_meta[call_id] = input;
    out_bodies->push_back(CodexToolCallBody("CodexBash", call_id, input));
    return true;
  }

  if (type == "exec_command_end") {
    const std::string call_id = ExtractCallId(payload).value_or(MakeCallId());
    nlohmann::json output = payload;
    output.erase("type");
    output.erase("call_id");
    output.erase("callId");

    auto command_meta = state->command_meta.find(call_id);
    if (command_meta != state->command_meta.end()) {
      for (auto it = command_meta->second.begin(); it != command_meta->second.end(); ++it) {
        if (!output.contains(it.key())) {
          output[it.key()] = it.value();
        }
      }
      state->command_meta.erase(command_meta);
    }

    if (!JsonFirstString(output, {"output", "stdout", "aggregated_output", "aggregatedOutput"}).has_value()) {
      auto delta_output = state->command_output_buffers.find(call_id);
      if (delta_output != state->command_output_buffers.end() && !delta_output->second.empty()) {
        output["output"] = delta_output->second;
      }
    }
    if (!JsonFirstString(output, {"output", "stdout"}).has_value()) {
      auto aggregated = JsonFirstString(output, {"aggregated_output", "aggregatedOutput"});
      if (aggregated.has_value()) {
        output["output"] = *aggregated;
      }
    }
    state->command_output_buffers.erase(call_id);
    out_bodies->push_back(CodexToolResultBody(call_id, output));
    return true;
  }

  if (type == "patch_apply_begin") {
    const std::string call_id = ExtractCallId(payload).value_or(MakeCallId());
    nlohmann::json input = {
        {"changes", ExtractChanges(payload.value("changes", nlohmann::json::object()))},
    };
    if (auto auto_approved = JsonFirstBool(payload, {"auto_approved", "autoApproved"}); auto_approved.has_value()) {
      input["auto_approved"] = *auto_approved;
    }
    state->patch_meta[call_id] = input;
    out_bodies->push_back(CodexToolCallBody("CodexPatch", call_id, input));
    return true;
  }

  if (type == "patch_apply_end") {
    const std::string call_id = ExtractCallId(payload).value_or(MakeCallId());
    nlohmann::json output = {
        {"stdout", JsonFirstString(payload, {"stdout"}).value_or("")},
        {"stderr", JsonFirstString(payload, {"stderr"}).value_or("")},
        {"success", JsonFirstBool(payload, {"success"}).value_or(false)},
    };
    state->patch_meta.erase(call_id);
    out_bodies->push_back(CodexToolResultBody(call_id, output));
    return true;
  }

  if (type == "mcp_tool_call_begin") {
    const std::string call_id = ExtractCallId(payload).value_or(MakeCallId());
    auto tool_name = BuildMcpToolName(payload);
    if (!tool_name.has_value()) {
      return true;
    }
    const nlohmann::json* invocation = JsonObjectField(payload, "invocation");
    nlohmann::json input = nlohmann::json::object();
    if (invocation != nullptr) {
      auto args_it = invocation->find("arguments");
      if (args_it != invocation->end()) {
        input = *args_it;
      } else {
        auto input_it = invocation->find("input");
        if (input_it != invocation->end()) {
          input = *input_it;
        }
      }
    } else {
      auto args_it = payload.find("arguments");
      if (args_it != payload.end()) {
        input = *args_it;
      } else {
        auto input_it = payload.find("input");
        if (input_it != payload.end()) {
          input = *input_it;
        }
      }
    }
    out_bodies->push_back(CodexToolCallBody(*tool_name, call_id, input));
    return true;
  }

  if (type == "mcp_tool_call_end") {
    const std::string call_id = ExtractCallId(payload).value_or(MakeCallId());
    bool is_error = false;
    nlohmann::json output = payload.value("result", nlohmann::json::object());
    if (output.is_object()) {
      auto ok_it = output.find("Ok");
      auto err_it = output.find("Err");
      if (ok_it != output.end()) {
        output = *ok_it;
      } else if (err_it != output.end()) {
        output = *err_it;
        is_error = true;
      }
    }
    out_bodies->push_back(CodexToolResultBody(call_id, output, is_error));
    return true;
  }

  if (type == "turn_diff") {
    auto diff = JsonFirstString(payload, {"unified_diff"});
    if (!diff.has_value() || diff->empty()) {
      return true;
    }
    if (state->last_diff == *diff) {
      return true;
    }
    state->last_diff = *diff;
    const std::string call_id = MakeCallId();
    out_bodies->push_back(CodexToolCallBody("CodexDiff", call_id, nlohmann::json{{"unified_diff", *diff}}));
    out_bodies->push_back(CodexToolResultBody(call_id, nlohmann::json{{"status", "completed"}}));
    return true;
  }

  if (type == "task_failed") {
    auto error = JsonFirstString(payload, {"error", "message"});
    if (error.has_value() && !error->empty()) {
      out_bodies->push_back(CodexMessageBody("Task failed: " + *error));
    }
    return true;
  }

  if (type == "task_complete" || type == "turn_aborted") {
    if (state->reasoning_tool_started) {
      std::string title;
      std::string content;
      if (!ParseReasoningTitleContent(state->live_reasoning_buffer, &title, &content)) {
        content = util::Trim(state->live_reasoning_buffer);
      }
      out_bodies->push_back(CodexToolResultBody(
          state->reasoning_tool_call_id, nlohmann::json{{"content", content}, {"status", "canceled"}}));
    } else if (!state->live_reasoning_buffer.empty()) {
      out_bodies->push_back(CodexReasoningBody(state->live_reasoning_buffer));
    }
    state->reasoning_tool_started = false;
    state->reasoning_tool_call_id.clear();
    state->reasoning_title.clear();
    if (!state->live_reasoning_buffer.empty()) {
      state->live_reasoning_buffer.clear();
    }
    return true;
  }

  if (type == "token_count") {
    nlohmann::json info = payload.value("info", nlohmann::json::object());
    if (!info.is_object()) {
      info = nlohmann::json::object();
    }
    if (info.empty()) {
      nlohmann::json fallback = payload;
      fallback.erase("type");
      fallback.erase("id");
      if (fallback.is_object()) {
        info = std::move(fallback);
      }
    }
    if (info.is_object() && !info.empty()) {
      out_bodies->push_back(CodexTokenCountBody(info));
    }
    return true;
  }

  if (type == "thread_started" || type == "task_started") {
    return true;
  }

  return false;
}

void DrainBufferedBodies(AgentOutputParseState* state, std::vector<nlohmann::json>* out_bodies) {
  if (state == nullptr || out_bodies == nullptr) {
    return;
  }

  for (const auto& [_, text] : state->agent_message_buffers) {
    if (!util::Trim(text).empty()) {
      out_bodies->push_back(CodexMessageBody(text));
    }
  }
  state->agent_message_buffers.clear();

  for (const auto& [_, text] : state->reasoning_buffers) {
    if (!util::Trim(text).empty()) {
      out_bodies->push_back(CodexReasoningBody(text));
    }
  }
  state->reasoning_buffers.clear();

  if (!util::Trim(state->live_reasoning_buffer).empty()) {
    if (state->reasoning_tool_started && !state->reasoning_tool_call_id.empty()) {
      std::string title;
      std::string content;
      if (!ParseReasoningTitleContent(state->live_reasoning_buffer, &title, &content)) {
        content = util::Trim(state->live_reasoning_buffer);
      }
      out_bodies->push_back(CodexToolResultBody(
          state->reasoning_tool_call_id, nlohmann::json{{"content", content}, {"status", "completed"}}));
    } else {
      out_bodies->push_back(CodexReasoningBody(state->live_reasoning_buffer));
    }
  }
  state->reasoning_tool_started = false;
  state->reasoning_tool_call_id.clear();
  state->reasoning_title.clear();
  state->live_reasoning_buffer.clear();
}

}  // namespace ferryman::codeagent::parser
