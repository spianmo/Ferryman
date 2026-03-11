#include "CodeAgentOutputParser.hpp"
#include "CodeAgentPolicy.hpp"
#include "ferryman/codeagent/CodeAgentManager.hpp"

#include "ferryman/util/Random.hpp"
#include "ferryman/util/StringUtil.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ferryman::codeagent {

namespace {

std::string EnvOrDefault(const char* key, const std::string& fallback) {
  const char* value = std::getenv(key);
  if (value == nullptr) {
    return fallback;
  }
  std::string trimmed = util::Trim(value);
  return trimmed.empty() ? fallback : trimmed;
}

struct CodexPermissionConfig {
  std::string approval_policy;
  std::string sandbox_mode;
  bool allow_default_override = false;
};

CodexPermissionConfig ResolveCodexPermissionConfig(const std::string& permission_mode) {
  const std::string normalized = policy::CanonicalizePermissionModeForFlavor(permission_mode, "codex");
  if (normalized == "read-only") {
    return CodexPermissionConfig{.approval_policy = "on-request", .sandbox_mode = "read-only"};
  }
  if (normalized == "full-access") {
    return CodexPermissionConfig{.approval_policy = "never", .sandbox_mode = "danger-full-access"};
  }
  return CodexPermissionConfig{
      .approval_policy = "on-request", .sandbox_mode = "workspace-write", .allow_default_override = true};
}

std::string ResolveGeminiApprovalMode(const std::string& permission_mode) {
  const std::string normalized = policy::CanonicalizePermissionModeForFlavor(permission_mode, "gemini");
  if (normalized == "auto-edit") {
    return "auto_edit";
  }
  if (normalized == "plan") {
    return "plan";
  }
  if (normalized == "yolo") {
    return "yolo";
  }
  return "default";
}

std::string FirstLine(std::string text) {
  text = util::Trim(text);
  const size_t pos = text.find('\n');
  if (pos != std::string::npos) {
    text = text.substr(0, pos);
  }
  constexpr size_t kMax = 160;
  if (text.size() > kMax) {
    text = text.substr(0, kMax) + "...";
  }
  return text;
}

void ReplaceAll(std::string* target, const std::string& needle, const std::string& replacement) {
  if (target == nullptr || needle.empty()) {
    return;
  }
  size_t start = 0;
  while ((start = target->find(needle, start)) != std::string::npos) {
    target->replace(start, needle.size(), replacement);
    start += replacement.size();
  }
}

bool HasCommandOption(const std::string& command, std::string_view option) {
  size_t pos = 0;
  while ((pos = command.find(option, pos)) != std::string::npos) {
    const bool before_ok =
        pos == 0 || std::isspace(static_cast<unsigned char>(command[pos - 1])) || command[pos - 1] == ';' ||
        command[pos - 1] == '&' || command[pos - 1] == '|';
    const size_t end = pos + option.size();
    const bool after_ok = end == command.size() || std::isspace(static_cast<unsigned char>(command[end])) ||
                          command[end] == '=';
    if (before_ok && after_ok) {
      return true;
    }
    pos = end;
  }
  return false;
}

void AppendCommandOption(std::string* command, std::string_view option, std::string_view value,
                         bool allow_if_missing_only = false) {
  if (command == nullptr || option.empty()) {
    return;
  }
  if (allow_if_missing_only && HasCommandOption(*command, option)) {
    return;
  }
  command->append(" ");
  command->append(option);
  if (!value.empty()) {
    command->append(" ");
    command->append(value);
  }
}

void AppendCodexCommandOption(std::string* command, std::string_view option, std::string_view value,
                              bool allow_if_missing_only = false) {
  if (command == nullptr || option.empty()) {
    return;
  }
  if (allow_if_missing_only && HasCommandOption(*command, option)) {
    return;
  }

  const size_t exec_pos = command->find(" exec");
  const size_t codex_pos = command->find("codex");
  if (exec_pos == std::string::npos || codex_pos == std::string::npos || codex_pos > exec_pos) {
    AppendCommandOption(command, option, value);
    return;
  }

  std::string insertion = " ";
  insertion.append(option);
  if (!value.empty()) {
    insertion.push_back(' ');
    insertion.append(value);
  }
  command->insert(exec_pos, insertion);
}

bool HasCodexConfigKey(const std::string& command, std::string_view key) {
  if (key.empty()) {
    return false;
  }
  const std::string pattern = std::string(key) + "=";
  return command.find(pattern) != std::string::npos;
}

std::string EscapeTomlString(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 16);
  for (char ch : value) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

std::string EscapeTomlLiteralString(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (char ch : value) {
    if (ch == '\'') {
      escaped += "''";
    } else {
      escaped.push_back(ch);
    }
  }
  return escaped;
}

std::string BuildTomlLiteralArray(const std::vector<std::string>& values) {
  std::string output = "[";
  for (size_t index = 0; index < values.size(); ++index) {
    if (index > 0) {
      output += ",";
    }
    output += "'";
    output += EscapeTomlLiteralString(values[index]);
    output += "'";
  }
  output += "]";
  return output;
}

const std::string& CodexTitleDeveloperInstruction() {
  static const std::string kInstruction = util::Trim(R"INSTR(
ALWAYS when you start a new chat, call the title tool to set a concise task title.
Prefer calling functions.hapi__change_title.
If that exact tool name is unavailable, call an equivalent alias such as hapi__change_title, mcp__hapi__change_title, or hapi_change_title.
If the task focus changes significantly later, call the title tool again with a better title.
)INSTR");
  return kInstruction;
}

const std::string& ClaudeTitleSystemPrompt() {
  static const std::string kInstruction = util::Trim(R"INSTR(
ALWAYS when you start a new chat - call the tool "mcp__hapi__change_title" exactly once to set a concise title.
Do not call title tools again in the same session.
)INSTR");
  return kInstruction;
}

std::filesystem::path ResolveCodexTitleMcpScriptPath(const std::filesystem::path& state_file_path) {
  const char* claude_override = std::getenv("FERRYMAN_CODEAGENT_CLAUDE_TITLE_MCP_SCRIPT");
  if (claude_override != nullptr) {
    const std::string trimmed = util::Trim(claude_override);
    if (!trimmed.empty()) {
      return std::filesystem::path(trimmed);
    }
  }
  const char* override_path = std::getenv("FERRYMAN_CODEAGENT_CODEX_TITLE_MCP_SCRIPT");
  if (override_path != nullptr) {
    const std::string trimmed = util::Trim(override_path);
    if (!trimmed.empty()) {
      return std::filesystem::path(trimmed);
    }
  }

  if (!state_file_path.empty()) {
    return state_file_path.parent_path() / "codeagent_title_mcp_server.py";
  }

  std::error_code ec;
  std::filesystem::path temp_path = std::filesystem::temp_directory_path(ec);
  if (ec || temp_path.empty()) {
    temp_path = std::filesystem::current_path(ec);
    if (ec || temp_path.empty()) {
      temp_path = ".";
    }
  }
  return temp_path / "ferryman_codeagent_title_mcp_server.py";
}

bool EnsureCodexTitleMcpScript(const std::filesystem::path& script_path) {
  if (script_path.empty()) {
    return false;
  }

  std::error_code ec;

  const std::filesystem::path parent = script_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return false;
    }
  }

  static const char* kScript = R"PY(#!/usr/bin/env python3
import json
import sys

PROTOCOL_FALLBACK = "2025-03-26"
TOOL_NAME = "change_title"
_USE_CONTENT_LENGTH = None


def send(message):
    payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    global _USE_CONTENT_LENGTH
    if _USE_CONTENT_LENGTH:
      sys.stdout.buffer.write(b"Content-Length: " + str(len(payload)).encode("ascii") + b"\r\n\r\n")
      sys.stdout.buffer.write(payload)
      sys.stdout.buffer.flush()
      return
    sys.stdout.write(payload.decode("utf-8") + "\n")
    sys.stdout.flush()


def send_error(request_id, code, message):
    send({
        "jsonrpc": "2.0",
        "id": request_id,
        "error": {"code": code, "message": message},
    })


def handle_message(payload):
    if not isinstance(payload, dict):
        return

    method = payload.get("method")
    request_id = payload.get("id")

    if method == "initialize":
        params = payload.get("params") if isinstance(payload.get("params"), dict) else {}
        protocol_version = params.get("protocolVersion")
        if not isinstance(protocol_version, str) or not protocol_version:
            protocol_version = PROTOCOL_FALLBACK
        send({
            "jsonrpc": "2.0",
            "id": request_id,
            "result": {
                "protocolVersion": protocol_version,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "ferryman-title-mcp", "version": "1.0.0"},
            },
        })
        return

    if method in ("notifications/initialized", "notifications/cancelled"):
        return

    if method == "ping":
        send({"jsonrpc": "2.0", "id": request_id, "result": {}})
        return

    if method == "tools/list":
        send({
            "jsonrpc": "2.0",
            "id": request_id,
            "result": {
                "tools": [
                    {
                        "name": TOOL_NAME,
                        "title": "Change Chat Title",
                        "description": "Change the title of the current chat session",
                        "inputSchema": {
                            "type": "object",
                            "properties": {
                                "title": {
                                    "type": "string",
                                    "description": "The new title for the chat session"
                                }
                            },
                            "required": ["title"]
                        }
                    }
                ]
            },
        })
        return

    if method == "tools/call":
        params = payload.get("params") if isinstance(payload.get("params"), dict) else {}
        tool_name = params.get("name")
        if tool_name != TOOL_NAME:
            send_error(request_id, -32601, f"Tool not found: {tool_name}")
            return

        arguments = params.get("arguments") if isinstance(params.get("arguments"), dict) else {}
        title = arguments.get("title")
        if not isinstance(title, str) or not title.strip():
            send({
                "jsonrpc": "2.0",
                "id": request_id,
                "result": {
                    "content": [{"type": "text", "text": "Failed to change chat title: missing title"}],
                    "isError": True
                },
            })
            return

        normalized_title = title.strip()
        send({
            "jsonrpc": "2.0",
            "id": request_id,
            "result": {
                "content": [
                    {
                        "type": "text",
                        "text": f"Successfully changed chat title to: \"{normalized_title}\""
                    }
                ],
                "isError": False
            },
        })
        return

    if request_id is not None:
        send_error(request_id, -32601, f"Method not found: {method}")


def parse_content_length_length(first_line):
    lower = first_line.lower()
    if not lower.startswith("content-length:"):
        return None
    raw_length = first_line.split(":", 1)[1].strip()
    try:
        length = int(raw_length)
    except ValueError:
        return None
    return length if length >= 0 else None


def read_message_stream():
    while True:
        header_or_line = sys.stdin.buffer.readline()
        if not header_or_line:
            return
        stripped = header_or_line.strip()
        if not stripped:
            continue

        decoded = None
        try:
            decoded = stripped.decode("utf-8")
        except UnicodeDecodeError:
            decoded = None

        content_length = parse_content_length_length(decoded) if decoded is not None else None
        if content_length is not None:
            global _USE_CONTENT_LENGTH
            if _USE_CONTENT_LENGTH is None:
                _USE_CONTENT_LENGTH = True
            while True:
                header = sys.stdin.buffer.readline()
                if not header:
                    return
                if header in (b"\r\n", b"\n"):
                    break
            body = sys.stdin.buffer.read(content_length)
            if body is None or len(body) < content_length:
                return
            try:
                text = body.decode("utf-8")
            except UnicodeDecodeError:
                continue
            yield text
            continue

        if _USE_CONTENT_LENGTH is None:
            _USE_CONTENT_LENGTH = False
        if decoded is None:
            continue
        yield decoded


def main():
    for raw_text in read_message_stream():
        line = raw_text.strip()
        if not line:
            continue
        try:
            payload = json.loads(line)
        except json.JSONDecodeError:
            continue

        if isinstance(payload, list):
            for entry in payload:
                handle_message(entry)
        else:
            handle_message(payload)


if __name__ == "__main__":
    main()
)PY";

  std::ofstream out(script_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  out << kScript;
  out.flush();
  if (!out.good()) {
    return false;
  }
  out.close();
  if (!out.good()) {
    return false;
  }

#if defined(__unix__) || defined(__APPLE__)
  std::filesystem::permissions(
      script_path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec |
          std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
          std::filesystem::perms::others_read | std::filesystem::perms::others_exec,
      std::filesystem::perm_options::replace, ec);
  if (ec) {
    return false;
  }
#endif

  return true;
}

}  // namespace

namespace {

std::string ToLowerCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::optional<std::string> JsonStringValue(const nlohmann::json& object,
                                           std::initializer_list<const char*> keys) {
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

std::optional<bool> JsonBoolValue(const nlohmann::json& object,
                                  std::initializer_list<const char*> keys) {
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

std::optional<int> JsonIntValue(const nlohmann::json& object,
                                std::initializer_list<const char*> keys) {
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
  }
  return std::nullopt;
}

const nlohmann::json* JsonObjectValue(const nlohmann::json& object,
                                      std::initializer_list<const char*> keys) {
  if (!object.is_object()) {
    return nullptr;
  }
  for (const char* key : keys) {
    if (key == nullptr) {
      continue;
    }
    auto it = object.find(key);
    if (it != object.end() && it->is_object()) {
      return &(*it);
    }
  }
  return nullptr;
}

bool IsBashLikeToolNameLocal(std::string_view name) {
  const std::string normalized = ToLowerCopy(util::Trim(std::string(name)));
  return normalized == "bash" || normalized == "codexbash";
}

bool IsEditLikeToolNameLocal(std::string_view name) {
  const std::string normalized = ToLowerCopy(util::Trim(std::string(name)));
  if (normalized.empty()) {
    return false;
  }
  static constexpr std::array<std::string_view, 8> kEditHints = {
      "edit", "write", "patch", "create", "delete", "remove", "replace", "multiedit",
  };
  for (std::string_view hint : kEditHints) {
    if (normalized.find(hint) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> BuildPermissionToolIdentifierLocal(const std::string& tool_name,
                                                              const nlohmann::json& input) {
  const std::string normalized_tool = util::Trim(tool_name);
  if (normalized_tool.empty()) {
    return std::nullopt;
  }
  if (IsBashLikeToolNameLocal(normalized_tool)) {
    std::string command = JsonStringValue(input, {"command"}).value_or("");
    command = util::Trim(command);
    if (command.empty()) {
      return std::string("Bash");
    }
    return "Bash(" + command + ")";
  }
  return normalized_tool;
}

bool EndsWithLocal(std::string_view value, std::string_view suffix) {
  return suffix.size() <= value.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::optional<std::string> ReadToolCommandLocal(const nlohmann::json& input) {
  if (!input.is_object()) {
    return std::nullopt;
  }
  auto command = JsonStringValue(input, {"command", "cmd", "input"});
  if (!command.has_value()) {
    return std::nullopt;
  }
  const std::string trimmed = util::Trim(*command);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  return trimmed;
}

bool IsAllowToolPatternMatchedLocal(const std::string& allow_tool, const std::string& request_tool,
                                    const nlohmann::json& request_input) {
  const std::string pattern = util::Trim(allow_tool);
  if (pattern.empty()) {
    return false;
  }
  if (pattern == "Bash" && IsBashLikeToolNameLocal(request_tool)) {
    return true;
  }
  if (pattern.size() > 6 && pattern.rfind("Bash(", 0) == 0 && pattern.back() == ')') {
    if (!IsBashLikeToolNameLocal(request_tool)) {
      return false;
    }
    auto command = ReadToolCommandLocal(request_input);
    if (!command.has_value()) {
      return false;
    }
    std::string body = pattern.substr(5, pattern.size() - 6);
    if (body.size() > 2 && EndsWithLocal(body, ":*")) {
      body.resize(body.size() - 2);
      return command->rfind(body, 0) == 0;
    }
    return *command == body;
  }
  return ToLowerCopy(pattern) == ToLowerCopy(util::Trim(request_tool));
}

bool IsPermissionAllowedForSessionLocal(const std::unordered_set<std::string>& allow_tools,
                                        const std::string& request_tool,
                                        const nlohmann::json& request_input) {
  for (const auto& allow_tool : allow_tools) {
    if (IsAllowToolPatternMatchedLocal(allow_tool, request_tool, request_input)) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> ResolveAutoPermissionDecisionLocal(const std::unordered_set<std::string>& allow_tools,
                                                              const std::string& mode,
                                                              const std::string& request_tool,
                                                              const nlohmann::json& request_input) {
  if (IsPermissionAllowedForSessionLocal(allow_tools, request_tool, request_input)) {
    return std::string("approved_for_session");
  }
  const std::string normalized_mode = policy::NormalizePermissionModeValue(mode);
  if (normalized_mode == "full-access" || normalized_mode == "yolo" || normalized_mode == "bypassPermissions" ||
      normalized_mode == "force" || normalized_mode == "allow") {
    return std::string("approved_for_session");
  }
  if ((normalized_mode == "acceptEdits" || normalized_mode == "auto-edit") &&
      IsEditLikeToolNameLocal(request_tool)) {
    return std::string("approved");
  }
  if (normalized_mode == "deny") {
    return std::string("denied");
  }
  return std::nullopt;
}

void MergePermissionAllowToolsLocal(std::unordered_set<std::string>* target,
                                    const std::vector<std::string>& allow_tools) {
  if (target == nullptr) {
    return;
  }
  for (const auto& raw_tool : allow_tools) {
    const std::string normalized_tool = util::Trim(raw_tool);
    if (!normalized_tool.empty()) {
      target->insert(normalized_tool);
    }
  }
}

std::string NormalizeItemType(std::string value) {
  value = ToLowerCopy(util::Trim(value));
  value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
    return ch == '_' || ch == '-' || std::isspace(ch);
  }), value.end());
  return value;
}

std::optional<std::string> ExtractItemId(const nlohmann::json& params) {
  if (!params.is_object()) {
    return std::nullopt;
  }
  if (auto direct = JsonStringValue(params, {"itemId", "item_id", "id"}); direct.has_value()) {
    return direct;
  }
  if (const nlohmann::json* item = JsonObjectValue(params, {"item"}); item != nullptr) {
    return JsonStringValue(*item, {"id", "itemId", "item_id"});
  }
  return std::nullopt;
}

std::string MakeStreamingBodyId(const char* kind, const std::string& item_id) {
  if (kind == nullptr || item_id.empty()) {
    return "codex-stream";
  }
  return "codex-stream-" + std::string(kind) + "-" + item_id;
}

constexpr std::string_view kPendingAgentMessageKey = "__pending_agent_message__";
constexpr std::string_view kPendingReasoningKey = "__pending_reasoning__";

std::string EnsureStreamingKey(std::unordered_map<std::string, std::string>* stream_keys,
                               const std::string& buffer_key, std::string_view prefix) {
  if (stream_keys == nullptr || buffer_key.empty()) {
    return std::string(prefix.empty() ? "stream" : prefix) + "-" + util::RandomHex(12);
  }
  auto existing = stream_keys->find(buffer_key);
  if (existing != stream_keys->end() && !existing->second.empty()) {
    return existing->second;
  }
  std::string generated = std::string(prefix.empty() ? "stream" : prefix) + "-" + util::RandomHex(12);
  (*stream_keys)[buffer_key] = generated;
  return generated;
}

template <typename BufferMap>
bool MovePendingBuffer(BufferMap* buffers, std::unordered_map<std::string, std::string>* stream_keys,
                       std::string_view pending_key, const std::string& item_id) {
  if (buffers == nullptr || item_id.empty()) {
    return false;
  }
  auto pending = buffers->find(std::string(pending_key));
  if (pending == buffers->end() || pending->second.empty()) {
    return false;
  }

  (*buffers)[item_id] = pending->second;
  buffers->erase(pending);

  if (stream_keys != nullptr) {
    auto pending_stream = stream_keys->find(std::string(pending_key));
    if (pending_stream != stream_keys->end()) {
      (*stream_keys)[item_id] = pending_stream->second;
      stream_keys->erase(pending_stream);
    }
  }
  return true;
}

bool StartsWithLocal(std::string_view value, std::string_view prefix) {
  return prefix.size() <= value.size() && value.compare(0, prefix.size(), prefix) == 0;
}

void MergeStreamingFragment(std::string* buffer, std::string_view fragment) {
  if (buffer == nullptr || fragment.empty()) {
    return;
  }
  if (buffer->empty()) {
    buffer->assign(fragment);
    return;
  }

  if (fragment.size() > buffer->size() && StartsWithLocal(fragment, *buffer)) {
    buffer->assign(fragment);
    return;
  }
  if (buffer->size() > fragment.size() && StartsWithLocal(*buffer, fragment)) {
    return;
  }

  const size_t max_overlap = std::min(buffer->size(), fragment.size());
  for (size_t overlap = max_overlap; overlap > 0; --overlap) {
    if (buffer->compare(buffer->size() - overlap, overlap, fragment.data(), overlap) == 0) {
      buffer->append(fragment.substr(overlap));
      return;
    }
  }

  buffer->append(fragment);
}

std::string JoinStringArray(const nlohmann::json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (!value.is_array()) {
    return std::string();
  }
  std::string output;
  for (const auto& entry : value) {
    if (!entry.is_string()) {
      continue;
    }
    if (!output.empty()) {
      output.push_back(' ');
    }
    output += entry.get<std::string>();
  }
  return output;
}

std::string FormatCodexTurnText(const std::string& text, const nlohmann::json& attachments) {
  std::vector<std::string> paths;
  if (attachments.is_array()) {
    for (const auto& attachment : attachments) {
      if (attachment.is_string()) {
        const std::string path = util::Trim(attachment.get<std::string>());
        if (!path.empty()) {
          paths.push_back(path);
        }
        continue;
      }
      if (!attachment.is_object()) {
        continue;
      }
      auto path = JsonStringValue(attachment, {"path", "name", "filename", "url"});
      if (path.has_value()) {
        const std::string trimmed = util::Trim(*path);
        if (!trimmed.empty()) {
          paths.push_back(trimmed);
        }
      }
    }
  }

  std::string attachment_text;
  for (const auto& path : paths) {
    if (!attachment_text.empty()) {
      attachment_text.push_back(' ');
    }
    attachment_text += "@" + path;
  }
  if (attachment_text.empty()) {
    return text;
  }
  if (util::Trim(text).empty()) {
    return attachment_text;
  }
  return attachment_text + "\n\n" + text;
}

std::string ResolveCodexAppServerApprovalPolicy(const std::string& permission_mode) {
  const std::string normalized = policy::CanonicalizePermissionModeForFlavor(permission_mode, "codex");
  if (normalized == "read-only") {
    return "never";
  }
  if (normalized == "full-access") {
    return "never";
  }
  return "untrusted";
}

std::string ResolveCodexAppServerSandboxMode(const std::string& permission_mode) {
  const std::string normalized = policy::CanonicalizePermissionModeForFlavor(permission_mode, "codex");
  if (normalized == "read-only") {
    return "read-only";
  }
  if (normalized == "full-access") {
    return "danger-full-access";
  }
  return "workspace-write";
}

nlohmann::json ResolveCodexAppServerSandboxPolicy(const std::string& permission_mode) {
  const std::string normalized = policy::CanonicalizePermissionModeForFlavor(permission_mode, "codex");
  if (normalized == "read-only") {
    return nlohmann::json{{"type", "readOnly"}};
  }
  if (normalized == "full-access") {
    return nlohmann::json{{"type", "dangerFullAccess"}};
  }
  return nlohmann::json{{"type", "workspaceWrite"}};
}

std::string ResolveCodexAppServerReasoningEffort(const std::string& value) {
  const std::string normalized = policy::NormalizeReasoningEffortValue(value);
  if (normalized == "low" || normalized == "medium" || normalized == "high") {
    return normalized;
  }
  if (normalized == "xhigh") {
    return "high";
  }
  return "auto";
}

std::string MapRuntimeDecisionToAppServerDecision(const std::string& decision) {
  if (decision == "approved_for_session") {
    return "acceptForSession";
  }
  if (decision == "abort") {
    return "cancel";
  }
  if (decision == "denied") {
    return "decline";
  }
  return "accept";
}

struct AppServerEventConverterState {
  std::unordered_map<std::string, std::string> agent_message_buffers;
  std::unordered_map<std::string, std::string> reasoning_buffers;
  std::unordered_map<std::string, std::string> command_output_buffers;
  std::unordered_map<std::string, std::string> agent_message_stream_keys;
  std::unordered_map<std::string, std::string> reasoning_stream_keys;
  std::unordered_map<std::string, nlohmann::json> command_meta;
  std::unordered_map<std::string, nlohmann::json> patch_meta;
};

bool IsDuplicateCodexWrapperEvent(std::string_view msg_type) {
  static constexpr std::array<std::string_view, 10> kDuplicatedWrapperEventTypes = {
      "item_started",
      "item_completed",
      "task_started",
      "task_complete",
      "turn_aborted",
      "agent_message_delta",
      "agent_message_content_delta",
      "reasoning_content_delta",
      "agent_reasoning_section_break",
      "exec_command_output_delta",
  };
  return std::find(kDuplicatedWrapperEventTypes.begin(), kDuplicatedWrapperEventTypes.end(), msg_type) !=
         kDuplicatedWrapperEventTypes.end();
}

std::vector<nlohmann::json> ConvertAppServerNotification(AppServerEventConverterState* state,
                                                         const std::string& method,
                                                         const nlohmann::json& params) {
  std::vector<nlohmann::json> events;
  const nlohmann::json params_object = params.is_object() ? params : nlohmann::json::object();

  if (method.rfind("codex/event/", 0) == 0) {
    const nlohmann::json msg = params_object.value("msg", nlohmann::json::object());
    if (!msg.is_object()) {
      return events;
    }

    const std::string msg_type = JsonStringValue(msg, {"type"}).value_or("");
    if (msg_type.empty()) {
      return events;
    }
    if (IsDuplicateCodexWrapperEvent(msg_type)) {
      return events;
    }

    if (msg_type == "item_started" || msg_type == "item_completed") {
      nlohmann::json nested_params = {
          {"item", msg.value("item", nlohmann::json::object())},
          {"itemId", JsonStringValue(msg, {"item_id", "itemId"}).value_or("")},
      };
      if (auto thread_id = JsonStringValue(msg, {"thread_id", "threadId"}); thread_id.has_value()) {
        nested_params["threadId"] = *thread_id;
      }
      if (auto turn_id = JsonStringValue(msg, {"turn_id", "turnId"}); turn_id.has_value()) {
        nested_params["turnId"] = *turn_id;
      }
      return ConvertAppServerNotification(state, msg_type == "item_started" ? "item/started" : "item/completed",
                                          nested_params);
    }

    if (msg_type == "task_started") {
      return ConvertAppServerNotification(state, "turn/started", nlohmann::json::object());
    }
    if (msg_type == "task_complete") {
      events.push_back({{"type", "task_complete"}});
      return events;
    }
    if (msg_type == "turn_aborted") {
      events.push_back({{"type", "turn_aborted"}});
      return events;
    }
    if (msg_type == "task_failed") {
      events.push_back(
          {{"type", "task_failed"},
           {"error", JsonStringValue(msg, {"error", "message", "reason"}).value_or("codex app-server error")}});
      return events;
    }

    if (msg_type == "agent_message_delta" || msg_type == "agent_message_content_delta") {
      nlohmann::json nested_params = {
          {"itemId", JsonStringValue(msg, {"item_id", "itemId", "id"}).value_or("agent-message")},
          {"delta", JsonStringValue(msg, {"delta", "text", "message"}).value_or("")},
      };
      return ConvertAppServerNotification(state, "item/agentMessage/delta", nested_params);
    }

    if (msg_type == "reasoning_content_delta") {
      nlohmann::json nested_params = {
          {"itemId", JsonStringValue(msg, {"item_id", "itemId", "id"}).value_or("reasoning")},
          {"delta", JsonStringValue(msg, {"delta", "text", "message"}).value_or("")},
      };
      return ConvertAppServerNotification(state, "item/reasoning/summaryTextDelta", nested_params);
    }

    if (msg_type == "agent_reasoning_section_break") {
      nlohmann::json nested_params = {
          {"itemId", JsonStringValue(msg, {"item_id", "itemId", "id"}).value_or("reasoning")},
      };
      return ConvertAppServerNotification(state, "item/reasoning/summaryPartAdded", nested_params);
    }

    if (msg_type == "exec_command_output_delta") {
      nlohmann::json nested_params = {
          {"itemId", JsonStringValue(msg, {"call_id", "callId", "item_id", "itemId", "id"}).value_or("")},
          {"delta", JsonStringValue(msg, {"delta", "output", "stdout", "text"}).value_or("")},
      };
      return ConvertAppServerNotification(state, "item/commandExecution/outputDelta", nested_params);
    }

    if (msg_type == "error") {
      const bool will_retry = JsonBoolValue(msg, {"will_retry", "willRetry"}).value_or(false);
      if (!will_retry) {
        events.push_back({{"type", "task_failed"},
                          {"error", JsonStringValue(msg, {"message", "reason"}).value_or("codex app-server error")}});
      }
      return events;
    }

    return events;
  }

  if (method == "thread/started" || method == "thread/resumed") {
    events.push_back({{"type", "thread_started"}});
    return events;
  }
  if (method == "turn/started") {
    events.push_back({{"type", "task_started"}});
    return events;
  }
  if (method == "turn/completed") {
    const nlohmann::json turn = params_object.value("turn", nlohmann::json::object());
    const std::string status = ToLowerCopy(JsonStringValue(params_object, {"status"})
                                               .value_or(JsonStringValue(turn, {"status"}).value_or("completed")));
    if (status == "interrupted" || status == "cancelled" || status == "canceled") {
      events.push_back({{"type", "turn_aborted"}});
    } else if (status == "failed" || status == "error") {
      events.push_back({{"type", "task_failed"},
                        {"error", JsonStringValue(params_object, {"error", "message", "reason"}).value_or("turn failed")}});
    } else {
      events.push_back({{"type", "task_complete"}});
    }
    return events;
  }
  if (method == "turn/diff/updated") {
    if (auto diff = JsonStringValue(params_object, {"diff", "unified_diff", "unifiedDiff"}); diff.has_value()) {
      events.push_back({{"type", "turn_diff"}, {"unified_diff", *diff}});
    }
    return events;
  }
  if (method == "thread/tokenUsage/updated") {
    nlohmann::json info = params_object.value("tokenUsage", params_object.value("token_usage", params_object));
    if (!info.is_object()) {
      info = nlohmann::json::object();
    }
    events.push_back({{"type", "token_count"}, {"info", info}});
    return events;
  }
  if (method == "error") {
    const bool will_retry = JsonBoolValue(params_object, {"will_retry", "willRetry"}).value_or(false);
    if (!will_retry) {
      events.push_back({{"type", "task_failed"},
                        {"error", JsonStringValue(params_object, {"message"}).value_or("codex app-server error")}});
    }
    return events;
  }

  if (method == "item/agentMessage/delta") {
    const std::string item_id = ExtractItemId(params_object).value_or(std::string(kPendingAgentMessageKey));
    if (auto delta = JsonStringValue(params_object, {"delta", "text", "message"}); delta.has_value()) {
      std::string& buffered = state->agent_message_buffers[item_id];
      MergeStreamingFragment(&buffered, *delta);
      if (!util::Trim(buffered).empty()) {
        const std::string stream_key = EnsureStreamingKey(&state->agent_message_stream_keys, item_id, "message");
        events.push_back({
            {"type", "agent_message"},
            {"id", MakeStreamingBodyId("message", stream_key)},
            {"message", buffered},
        });
      }
    }
    return events;
  }
  if (method == "item/reasoning/textDelta" || method == "item/reasoning/summaryTextDelta") {
    const std::string item_id = ExtractItemId(params_object).value_or(std::string(kPendingReasoningKey));
    if (auto delta = JsonStringValue(params_object, {"delta", "text", "message"}); delta.has_value()) {
      MergeStreamingFragment(&state->reasoning_buffers[item_id], *delta);
      events.push_back({{"type", "agent_reasoning_delta"}, {"delta", *delta}});
    }
    return events;
  }
  if (method == "item/reasoning/summaryPartAdded") {
    events.push_back({{"type", "agent_reasoning_section_break"}});
    return events;
  }
  if (method == "item/commandExecution/outputDelta") {
    if (auto item_id = ExtractItemId(params_object); item_id.has_value()) {
      if (auto delta = JsonStringValue(params_object, {"delta", "text", "output", "stdout"}); delta.has_value()) {
        MergeStreamingFragment(&state->command_output_buffers[*item_id], *delta);
      }
    }
    return events;
  }

  if (method != "item/started" && method != "item/completed") {
    return events;
  }

  const nlohmann::json item = params_object.value("item", params_object);
  if (!item.is_object()) {
    return events;
  }
  const std::string item_id = ExtractItemId(params_object).value_or(JsonStringValue(item, {"id", "itemId", "item_id"}).value_or(""));
  const std::string item_type = NormalizeItemType(JsonStringValue(item, {"type", "itemType", "kind"}).value_or(""));
  if (item_id.empty() || item_type.empty()) {
    return events;
  }

  if (item_type == "agentmessage") {
    if (method == "item/completed") {
      MovePendingBuffer(&state->agent_message_buffers, &state->agent_message_stream_keys, kPendingAgentMessageKey,
                        item_id);
      std::string text = JsonStringValue(item, {"text", "message"}).value_or("");
      if (text.empty()) {
        text = state->agent_message_buffers[item_id];
      }
      if (!text.empty()) {
        const std::string stream_key = EnsureStreamingKey(&state->agent_message_stream_keys, item_id, "message");
        events.push_back({
            {"type", "agent_message"},
            {"id", MakeStreamingBodyId("message", stream_key)},
            {"message", text},
        });
      }
      state->agent_message_buffers.erase(item_id);
    }
    return events;
  }

  if (item_type == "reasoning") {
    if (method == "item/completed") {
      MovePendingBuffer(&state->reasoning_buffers, &state->reasoning_stream_keys, kPendingReasoningKey, item_id);
      std::string text = JsonStringValue(item, {"text", "message"}).value_or("");
      if (text.empty()) {
        text = state->reasoning_buffers[item_id];
      }
      if (!text.empty()) {
        events.push_back({{"type", "agent_reasoning"}, {"text", text}});
      }
      state->reasoning_buffers.erase(item_id);
    }
    return events;
  }

  if (item_type == "commandexecution") {
    if (method == "item/started") {
      nlohmann::json meta = nlohmann::json::object();
      const std::string command = JoinStringArray(item.value("command", item.value("cmd", item.value("args", nlohmann::json()))));
      if (!util::Trim(command).empty()) {
        meta["command"] = command;
      }
      if (auto cwd = JsonStringValue(item, {"cwd", "workingDirectory", "working_directory"}); cwd.has_value()) {
        meta["cwd"] = *cwd;
      }
      if (auto auto_approved = JsonBoolValue(item, {"autoApproved", "auto_approved"}); auto_approved.has_value()) {
        meta["auto_approved"] = *auto_approved;
      }
      state->command_meta[item_id] = meta;
      nlohmann::json event = {{"type", "exec_command_begin"}, {"call_id", item_id}};
      for (auto it = meta.begin(); it != meta.end(); ++it) {
        event[it.key()] = it.value();
      }
      events.push_back(std::move(event));
    } else {
      nlohmann::json meta = state->command_meta[item_id];
      nlohmann::json event = {{"type", "exec_command_end"}, {"call_id", item_id}};
      for (auto it = meta.begin(); it != meta.end(); ++it) {
        event[it.key()] = it.value();
      }
      if (auto output = JsonStringValue(item, {"output", "result", "stdout"}); output.has_value()) {
        event["output"] = *output;
      } else if (auto buffer_it = state->command_output_buffers.find(item_id); buffer_it != state->command_output_buffers.end()) {
        event["output"] = buffer_it->second;
      }
      if (auto stderr_text = JsonStringValue(item, {"stderr"}); stderr_text.has_value()) {
        event["stderr"] = *stderr_text;
      }
      if (auto error_text = JsonStringValue(item, {"error"}); error_text.has_value()) {
        event["error"] = *error_text;
      }
      if (auto exit_code = JsonIntValue(item, {"exitCode", "exit_code", "exitcode"}); exit_code.has_value()) {
        event["exit_code"] = *exit_code;
      }
      if (auto status = JsonStringValue(item, {"status"}); status.has_value()) {
        event["status"] = *status;
      }
      events.push_back(std::move(event));
      state->command_meta.erase(item_id);
      state->command_output_buffers.erase(item_id);
    }
    return events;
  }

  if (item_type == "filechange") {
    if (method == "item/started") {
      nlohmann::json meta = nlohmann::json::object();
      auto changes_it = item.find("changes");
      if (changes_it != item.end()) {
        meta["changes"] = *changes_it;
      }
      if (auto auto_approved = JsonBoolValue(item, {"autoApproved", "auto_approved"}); auto_approved.has_value()) {
        meta["auto_approved"] = *auto_approved;
      }
      state->patch_meta[item_id] = meta;
      nlohmann::json event = {{"type", "patch_apply_begin"}, {"call_id", item_id}};
      for (auto it = meta.begin(); it != meta.end(); ++it) {
        event[it.key()] = it.value();
      }
      events.push_back(std::move(event));
    } else {
      nlohmann::json meta = state->patch_meta[item_id];
      nlohmann::json event = {{"type", "patch_apply_end"}, {"call_id", item_id}, {"success", false}};
      for (auto it = meta.begin(); it != meta.end(); ++it) {
        event[it.key()] = it.value();
      }
      if (auto stdout_text = JsonStringValue(item, {"stdout", "output"}); stdout_text.has_value()) {
        event["stdout"] = *stdout_text;
      }
      if (auto stderr_text = JsonStringValue(item, {"stderr"}); stderr_text.has_value()) {
        event["stderr"] = *stderr_text;
      }
      if (auto success = JsonBoolValue(item, {"success", "ok", "applied"}); success.has_value()) {
        event["success"] = *success;
      } else if (auto status = JsonStringValue(item, {"status"}); status.has_value()) {
        event["success"] = ToLowerCopy(*status) == "completed";
      }
      events.push_back(std::move(event));
      state->patch_meta.erase(item_id);
    }
    return events;
  }

  if (item_type == "mcptoolcall") {
    nlohmann::json event = {
        {"type", method == "item/completed" ? "mcp_tool_call_end" : "mcp_tool_call_begin"},
        {"call_id", item_id},
    };

    if (const nlohmann::json* invocation = JsonObjectValue(item, {"invocation"}); invocation != nullptr) {
      event["invocation"] = *invocation;
    }
    if (auto server = JsonStringValue(item, {"server", "server_name"}); server.has_value()) {
      event["server"] = *server;
    }
    if (auto tool = JsonStringValue(item, {"tool", "tool_name"}); tool.has_value()) {
      event["tool"] = *tool;
    }

    if (method == "item/completed") {
      auto result_it = item.find("result");
      if (result_it != item.end()) {
        event["result"] = *result_it;
      } else if (auto output_it = item.find("output"); output_it != item.end()) {
        event["result"] = *output_it;
      } else if (auto content_it = item.find("content"); content_it != item.end()) {
        event["result"] = *content_it;
      } else if (auto error_it = item.find("error"); error_it != item.end()) {
        event["result"] = nlohmann::json{{"Err", *error_it}};
      }
    } else if (!event.contains("invocation")) {
      auto args_it = item.find("arguments");
      if (args_it != item.end()) {
        event["arguments"] = *args_it;
      } else if (auto input_it = item.find("input"); input_it != item.end()) {
        event["input"] = *input_it;
      }
    }

    events.push_back(std::move(event));
    return events;
  }

  return events;
}

}  // namespace

struct CodeAgentManager::SessionRunnerState {
  struct PendingResponse {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    nlohmann::json result;
    std::string error;
  };

  struct PendingPermission {
    std::string tool_name;
    nlohmann::json input;
    std::mutex mu;
    std::condition_variable cv;
    bool resolved = false;
    std::string response_decision;
  };

  struct TurnTask {
    std::uint64_t generation = 0;
    std::string prompt;
    nlohmann::json attachments = nlohmann::json::array();
    std::string continuation_context;
  };

  CodeAgentManager* manager = nullptr;
  std::string session_id;
  std::filesystem::path cwd;
  std::mutex mu;
  std::mutex write_mu;
  std::condition_variable cv;
  bool started = false;
  bool stop = false;
  bool turn_active = false;
  bool turn_done = false;
  std::uint64_t active_generation = 0;
  std::string current_turn_id;
  std::string thread_id;
  std::string summary_hint;
  parser::AgentOutputParseState parse_state;
  AppServerEventConverterState event_converter;
  std::deque<TurnTask> queue;
  int next_request_id = 1;
  std::unordered_map<int, std::shared_ptr<PendingResponse>> pending_responses;
  std::unordered_map<std::string, std::shared_ptr<PendingPermission>> pending_permissions;
  std::thread worker_thread;
  std::thread reader_thread;
#if defined(__unix__) || defined(__APPLE__)
  pid_t child_pid = -1;
  int stdin_fd = -1;
  int stdout_fd = -1;
#endif

  explicit SessionRunnerState(CodeAgentManager* owner, const SessionRecord& session)
      : manager(owner), session_id(session.id), cwd(session.path) {}

  ~SessionRunnerState() { Stop(); }

  bool Start(std::string* error);
  void Stop();
  bool EnqueueTurn(std::uint64_t generation, const std::string& prompt, const nlohmann::json& attachments,
                   const std::string& continuation_context, std::string* error);
  bool Interrupt(std::string* error);

 private:
  bool SpawnProcess(std::string* error);
  bool SendRequest(const std::string& method, const nlohmann::json& params, nlohmann::json* result,
                   int timeout_ms, std::string* error);
  bool WriteJsonLine(const nlohmann::json& payload);
  void ReaderLoop();
  void WorkerLoop();
  void HandleNotification(const std::string& method, const nlohmann::json& params);
  nlohmann::json HandleIncomingRequest(const std::string& method, const nlohmann::json& params);
  void HandleResponse(const nlohmann::json& payload);
  bool EnsureThreadStarted(const TurnTask& task, bool* fresh_thread, std::string* error);
  nlohmann::json BuildThreadStartParamsLocked(const SessionRecord& session) const;
  nlohmann::json BuildTurnStartParamsLocked(const SessionRecord& session, const std::string& input_text) const;
  void EmitBodies(std::uint64_t generation, const std::vector<nlohmann::json>& bodies);
  void CompleteTurn(const std::string& fallback_summary);
  void FailTurn(const std::string& error_message);
  void RejectPendingRequests(const std::string& message);
};

bool CodeAgentManager::SessionRunnerState::Start(std::string* error) {
  {
    std::lock_guard<std::mutex> lock(mu);
    if (started) {
      return true;
    }
  }

  if (!SpawnProcess(error)) {
    return false;
  }

  nlohmann::json init_result;
  if (!SendRequest("initialize",
                   nlohmann::json{{"clientInfo", {{"name", "ferryman-codex-runner"},
                                                   {"version", "1.0.0"}}}},
                   &init_result, 30000, error)) {
    Stop();
    return false;
  }
  WriteJsonLine(nlohmann::json{{"method", "initialized"}});

  {
    std::lock_guard<std::mutex> lock(mu);
    started = true;
    worker_thread = std::thread([this]() { WorkerLoop(); });
  }
  return true;
}

void CodeAgentManager::SessionRunnerState::RejectPendingRequests(const std::string& message) {
  std::unordered_map<int, std::shared_ptr<PendingResponse>> responses;
  std::unordered_map<std::string, std::shared_ptr<PendingPermission>> permissions;
  {
    std::lock_guard<std::mutex> lock(mu);
    responses.swap(pending_responses);
    permissions.swap(pending_permissions);
  }

  for (auto& [_, pending] : responses) {
    std::lock_guard<std::mutex> pending_lock(pending->mu);
    pending->done = true;
    pending->error = message;
    pending->cv.notify_all();
  }
  for (auto& [_, pending] : permissions) {
    std::lock_guard<std::mutex> pending_lock(pending->mu);
    pending->resolved = true;
    pending->response_decision = "cancel";
    pending->cv.notify_all();
  }
}

void CodeAgentManager::SessionRunnerState::Stop() {
  bool already_stopped = false;
#if defined(__unix__) || defined(__APPLE__)
  int local_stdin_fd = -1;
  int local_stdout_fd = -1;
  pid_t local_pid = -1;
#endif
  {
    std::lock_guard<std::mutex> lock(mu);
    already_stopped = stop;
    stop = true;
    cv.notify_all();
#if defined(__unix__) || defined(__APPLE__)
    local_stdin_fd = stdin_fd;
    stdin_fd = -1;
    local_stdout_fd = stdout_fd;
    stdout_fd = -1;
    local_pid = child_pid;
    child_pid = -1;
#endif
  }

  if (already_stopped) {
    if (worker_thread.joinable()) {
      worker_thread.join();
    }
    if (reader_thread.joinable()) {
      reader_thread.join();
    }
    return;
  }

  RejectPendingRequests("runner stopped");

#if defined(__unix__) || defined(__APPLE__)
  if (local_stdin_fd >= 0) {
    ::close(local_stdin_fd);
  }
  if (local_pid > 0) {
    ::kill(local_pid, SIGTERM);
  }
#endif

  if (worker_thread.joinable()) {
    worker_thread.join();
  }
  if (reader_thread.joinable()) {
    reader_thread.join();
  }

#if defined(__unix__) || defined(__APPLE__)
  if (local_stdout_fd >= 0) {
    ::close(local_stdout_fd);
  }
  if (local_pid > 0) {
    int status = 0;
    (void)::waitpid(local_pid, &status, 0);
  }
#endif
}

bool CodeAgentManager::SessionRunnerState::EnqueueTurn(std::uint64_t generation, const std::string& prompt,
                                                       const nlohmann::json& attachments,
                                                       const std::string& continuation_context,
                                                       std::string* error) {
  {
    std::lock_guard<std::mutex> lock(mu);
    if (stop) {
      if (error != nullptr) {
        *error = "runner stopped";
      }
      return false;
    }
    queue.push_back(TurnTask{.generation = generation,
                             .prompt = prompt,
                             .attachments = attachments,
                             .continuation_context = continuation_context});
  }
  cv.notify_all();
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool CodeAgentManager::SessionRunnerState::Interrupt(std::string* error) {
  std::string local_thread_id;
  std::string local_turn_id;
  std::unordered_map<std::string, std::shared_ptr<PendingPermission>> permissions;
  {
    std::lock_guard<std::mutex> lock(mu);
    local_thread_id = thread_id;
    local_turn_id = current_turn_id;
    queue.clear();
    permissions.swap(pending_permissions);
  }

  for (auto& entry : permissions) {
    if (!entry.second) {
      continue;
    }
    std::lock_guard<std::mutex> pending_lock(entry.second->mu);
    entry.second->resolved = true;
    entry.second->response_decision = "cancel";
    entry.second->cv.notify_all();
  }

  if (local_thread_id.empty() || local_turn_id.empty()) {
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  nlohmann::json result;
  if (!SendRequest("turn/interrupt", nlohmann::json{{"threadId", local_thread_id}, {"turnId", local_turn_id}},
                   &result, 30000, error)) {
    return false;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool CodeAgentManager::SessionRunnerState::SpawnProcess(std::string* error) {
#if !defined(__unix__) && !defined(__APPLE__)
  if (error != nullptr) {
    *error = "codex app-server runner is only supported on Unix-like platforms";
  }
  return false;
#else
  int stdin_pipe[2] = {-1, -1};
  int stdout_pipe[2] = {-1, -1};
  if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0) {
    if (stdin_pipe[0] >= 0) ::close(stdin_pipe[0]);
    if (stdin_pipe[1] >= 0) ::close(stdin_pipe[1]);
    if (stdout_pipe[0] >= 0) ::close(stdout_pipe[0]);
    if (stdout_pipe[1] >= 0) ::close(stdout_pipe[1]);
    if (error != nullptr) {
      *error = "failed to create pipes for codex app-server";
    }
    return false;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(stdin_pipe[0]);
    ::close(stdin_pipe[1]);
    ::close(stdout_pipe[0]);
    ::close(stdout_pipe[1]);
    if (error != nullptr) {
      *error = "failed to fork codex app-server";
    }
    return false;
  }

  if (pid == 0) {
    ::dup2(stdin_pipe[0], STDIN_FILENO);
    ::dup2(stdout_pipe[1], STDOUT_FILENO);
    ::close(stdin_pipe[0]);
    ::close(stdin_pipe[1]);
    ::close(stdout_pipe[0]);
    ::close(stdout_pipe[1]);
    ::execlp("codex", "codex", "app-server", static_cast<char*>(nullptr));
    _exit(127);
  }

  ::close(stdin_pipe[0]);
  ::close(stdout_pipe[1]);
  stdin_fd = stdin_pipe[1];
  stdout_fd = stdout_pipe[0];
  child_pid = pid;
  reader_thread = std::thread([this]() { ReaderLoop(); });
  if (error != nullptr) {
    error->clear();
  }
  return true;
#endif
}

bool CodeAgentManager::SessionRunnerState::WriteJsonLine(const nlohmann::json& payload) {
#if !defined(__unix__) && !defined(__APPLE__)
  return false;
#else
  const std::string line = payload.dump() + "\n";
  std::lock_guard<std::mutex> lock(write_mu);
  if (stdin_fd < 0) {
    return false;
  }
  const ssize_t written = ::write(stdin_fd, line.data(), line.size());
  return written == static_cast<ssize_t>(line.size());
#endif
}

bool CodeAgentManager::SessionRunnerState::SendRequest(const std::string& method, const nlohmann::json& params,
                                                       nlohmann::json* result, int timeout_ms,
                                                       std::string* error) {
  const auto pending = std::make_shared<PendingResponse>();
  int request_id = 0;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (stop) {
      if (error != nullptr) {
        *error = "runner stopped";
      }
      return false;
    }
    request_id = next_request_id++;
    pending_responses[request_id] = pending;
  }

  if (!WriteJsonLine(nlohmann::json{{"id", request_id}, {"method", method}, {"params", params}})) {
    std::lock_guard<std::mutex> lock(mu);
    pending_responses.erase(request_id);
    if (error != nullptr) {
      *error = "failed to write request to codex app-server";
    }
    return false;
  }

  std::unique_lock<std::mutex> pending_lock(pending->mu);
  if (timeout_ms > 0) {
    const bool completed = pending->cv.wait_for(pending_lock, std::chrono::milliseconds(timeout_ms), [&]() {
      return pending->done;
    });
    if (!completed) {
      if (error != nullptr) {
        *error = "codex app-server request timed out";
      }
      std::lock_guard<std::mutex> lock(mu);
      pending_responses.erase(request_id);
      return false;
    }
  } else {
    pending->cv.wait(pending_lock, [&]() { return pending->done; });
  }

  if (!pending->error.empty()) {
    if (error != nullptr) {
      *error = pending->error;
    }
    return false;
  }
  if (result != nullptr) {
    *result = pending->result;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

void CodeAgentManager::SessionRunnerState::HandleResponse(const nlohmann::json& payload) {
  if (!payload.is_object()) {
    return;
  }
  auto id_it = payload.find("id");
  if (id_it == payload.end() || !id_it->is_number_integer()) {
    return;
  }

  std::shared_ptr<PendingResponse> pending;
  {
    std::lock_guard<std::mutex> lock(mu);
    auto pending_it = pending_responses.find(id_it->get<int>());
    if (pending_it == pending_responses.end()) {
      return;
    }
    pending = pending_it->second;
    pending_responses.erase(pending_it);
  }

  std::lock_guard<std::mutex> pending_lock(pending->mu);
  pending->done = true;
  if (auto error_it = payload.find("error"); error_it != payload.end() && error_it->is_object()) {
    pending->error = JsonStringValue(*error_it, {"message"}).value_or("codex app-server request failed");
  } else {
    auto result_it = payload.find("result");
    pending->result = result_it == payload.end() ? nlohmann::json::object() : *result_it;
  }
  pending->cv.notify_all();
}

void CodeAgentManager::SessionRunnerState::EmitBodies(std::uint64_t generation,
                                                      const std::vector<nlohmann::json>& bodies) {
  for (const auto& body : bodies) {
    std::string body_summary;
    if (manager->EmitCodexBodyMessage(session_id, generation, body, &body_summary) && !util::Trim(body_summary).empty()) {
      std::lock_guard<std::mutex> lock(mu);
      summary_hint = body_summary;
    }
  }
}

void CodeAgentManager::SessionRunnerState::CompleteTurn(const std::string& fallback_summary) {
  std::uint64_t generation = 0;
  std::string summary_source;
  {
    std::lock_guard<std::mutex> lock(mu);
    generation = active_generation;
    summary_source = util::Trim(summary_hint).empty() ? fallback_summary : summary_hint;
    if (summary_source.empty()) {
      summary_source = "(empty output)";
    }
    turn_active = false;
    turn_done = true;
    current_turn_id.clear();
    summary_hint.clear();
    cv.notify_all();
  }

  std::lock_guard<std::mutex> manager_lock(manager->mu_);
  auto session_it = manager->sessions_by_id_.find(session_id);
  if (session_it == manager->sessions_by_id_.end()) {
    return;
  }
  SessionRecord& session = session_it->second;
  if (session.generation != generation) {
    return;
  }
  session.summary_text = FirstLine(summary_source);
  session.summary_updated_at_ms = CodeAgentManager::NowMs();
  session.updated_at_ms = session.summary_updated_at_ms;
  session.thinking = false;
  manager->PushEventLocked(session.ns,
                           {{"type", "session-updated"},
                            {"namespace", session.ns},
                            {"sessionId", session.id},
                            {"data", manager->BuildSessionJsonLocked(session)}},
                           session.id);
  manager->PersistStateLocked();
}

void CodeAgentManager::SessionRunnerState::FailTurn(const std::string& error_message) {
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(mu);
    generation = active_generation;
  }

  std::vector<nlohmann::json> bodies;
  parser::ParseAgentOutputJson(nlohmann::json{{"type", "task_failed"}, {"error", error_message}}, &parse_state,
                               &bodies);
  parser::DrainBufferedBodies(&parse_state, &bodies);
  EmitBodies(generation, bodies);
  CompleteTurn(error_message.empty() ? std::string("task failed") : error_message);
}

nlohmann::json CodeAgentManager::SessionRunnerState::BuildThreadStartParamsLocked(const SessionRecord& session) const {
  const std::string title_instructions = CodexTitleDeveloperInstruction();
  nlohmann::json params = {
      {"cwd", session.path.string()},
      {"approvalPolicy", ResolveCodexAppServerApprovalPolicy(session.permission_mode)},
      {"sandbox", ResolveCodexAppServerSandboxMode(session.permission_mode)},
      {"baseInstructions", title_instructions},
      {"developerInstructions", title_instructions},
  };
  if (!util::Trim(session.model).empty()) {
    params["model"] = session.model;
  }
  if (session.codex_fast) {
    params["serviceTier"] = "fast";
  }
  nlohmann::json config = nlohmann::json::object();
  config["developer_instructions"] = title_instructions;
  const auto title_mcp_script = ResolveCodexTitleMcpScriptPath(manager->state_file_path_);
  if (EnsureCodexTitleMcpScript(title_mcp_script)) {
    const std::string mcp_command = EnvOrDefault("FERRYMAN_CODEAGENT_CODEX_TITLE_MCP_COMMAND", "python3");
    config["mcp_servers.hapi"] = {
        {"command", mcp_command},
        {"args", nlohmann::json::array({"-u", title_mcp_script.string()})},
    };
  }
  if (!config.empty()) {
    params["config"] = std::move(config);
  }
  return params;
}

nlohmann::json CodeAgentManager::SessionRunnerState::BuildTurnStartParamsLocked(const SessionRecord& session,
                                                                                 const std::string& input_text) const {
  nlohmann::json params = {
      {"threadId", thread_id},
      {"input", nlohmann::json::array({{{"type", "text"}, {"text", input_text}}})},
      {"approvalPolicy", ResolveCodexAppServerApprovalPolicy(session.permission_mode)},
      {"sandboxPolicy", ResolveCodexAppServerSandboxPolicy(session.permission_mode)},
  };
  if (!util::Trim(session.model).empty()) {
    params["model"] = session.model;
  }
  if (!session.model_reasoning_effort.empty()) {
    params["effort"] = ResolveCodexAppServerReasoningEffort(session.model_reasoning_effort);
  }
  if (session.codex_fast) {
    params["serviceTier"] = "fast";
  }
  return params;
}

bool CodeAgentManager::SessionRunnerState::EnsureThreadStarted(const TurnTask& task, bool* fresh_thread,
                                                               std::string* error) {
  (void)task;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (!thread_id.empty()) {
      if (fresh_thread != nullptr) {
        *fresh_thread = false;
      }
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }
  }

  SessionRecord session_snapshot;
  {
    std::lock_guard<std::mutex> manager_lock(manager->mu_);
    auto session_it = manager->sessions_by_id_.find(session_id);
    if (session_it == manager->sessions_by_id_.end()) {
      if (error != nullptr) {
        *error = "session not found";
      }
      return false;
    }
    session_snapshot = session_it->second;
  }

  nlohmann::json result;
  if (!SendRequest("thread/start", BuildThreadStartParamsLocked(session_snapshot), &result, 30000, error)) {
    return false;
  }

  const nlohmann::json thread = result.value("thread", nlohmann::json::object());
  const std::string created_thread_id = JsonStringValue(thread, {"id"}).value_or("");
  if (created_thread_id.empty()) {
    if (error != nullptr) {
      *error = "codex app-server did not return a thread id";
    }
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mu);
    thread_id = created_thread_id;
  }
  if (fresh_thread != nullptr) {
    *fresh_thread = true;
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

void CodeAgentManager::SessionRunnerState::HandleNotification(const std::string& method,
                                                              const nlohmann::json& params) {
  std::vector<nlohmann::json> converted;
  std::uint64_t generation = 0;
  bool terminal_event = false;
  std::string fallback_summary;

  {
    std::lock_guard<std::mutex> lock(mu);
    generation = active_generation;
    converted = ConvertAppServerNotification(&event_converter, method, params);
  }

  for (const auto& event : converted) {
    std::vector<nlohmann::json> bodies;
    {
      std::lock_guard<std::mutex> lock(mu);
      parser::ParseAgentOutputJson(event, &parse_state, &bodies);
    }
    EmitBodies(generation, bodies);

    const std::string type = JsonStringValue(event, {"type"}).value_or("");
    if (type == "task_complete" || type == "task_failed" || type == "turn_aborted") {
      terminal_event = true;
      if (type == "task_failed") {
        fallback_summary = JsonStringValue(event, {"error", "message"}).value_or("task failed");
      }
    }
  }

  if (terminal_event) {
    std::vector<nlohmann::json> buffered;
    {
      std::lock_guard<std::mutex> lock(mu);
      parser::DrainBufferedBodies(&parse_state, &buffered);
      event_converter = AppServerEventConverterState{};
    }
    EmitBodies(generation, buffered);
    CompleteTurn(fallback_summary);
  }
}

nlohmann::json CodeAgentManager::SessionRunnerState::HandleIncomingRequest(const std::string& method,
                                                                           const nlohmann::json& params) {
  const std::string request_id = ExtractItemId(params).value_or("req-" + util::RandomHex(12));
  std::string tool_name;
  nlohmann::json input = nlohmann::json::object();

  if (method == "item/commandExecution/requestApproval") {
    tool_name = "CodexBash";
    input["message"] = JsonStringValue(params, {"reason"}).value_or("");
    auto command_it = params.find("command");
    if (command_it != params.end()) {
      input["command"] = command_it->is_string() ? nlohmann::json(command_it->get<std::string>()) : *command_it;
    }
    if (auto cwd_value = JsonStringValue(params, {"cwd"}); cwd_value.has_value()) {
      input["cwd"] = *cwd_value;
    }
    input["auto_approved"] = false;
  } else if (method == "item/fileChange/requestApproval") {
    tool_name = "CodexPatch";
    input["message"] = JsonStringValue(params, {"reason"}).value_or("");
    if (auto grant_root = JsonStringValue(params, {"grantRoot"}); grant_root.has_value()) {
      input["grantRoot"] = *grant_root;
    }
    input["auto_approved"] = false;
  } else if (method == "item/tool/requestUserInput") {
    return nlohmann::json{{"decision", "cancel"}};
  } else {
    return nlohmann::json{{"decision", "cancel"}};
  }

  std::shared_ptr<PendingPermission> pending;
  {
    std::lock_guard<std::mutex> manager_lock(manager->mu_);
    auto session_it = manager->sessions_by_id_.find(session_id);
    if (session_it == manager->sessions_by_id_.end()) {
      return nlohmann::json{{"decision", "cancel"}};
    }

    SessionRecord& session = session_it->second;
    const std::int64_t now = CodeAgentManager::NowMs();
    auto auto_decision = ResolveAutoPermissionDecisionLocal(session.permission_allow_tools, session.permission_mode,
                                                            tool_name, input);
    if (auto_decision.has_value()) {
      if (!session.agent_state_completed_requests.is_object()) {
        session.agent_state_completed_requests = nlohmann::json::object();
      }
      nlohmann::json completed = {
          {"tool", tool_name},
          {"arguments", input},
          {"createdAt", now},
          {"completedAt", now},
          {"status", *auto_decision == "denied" ? "denied" : "approved"},
          {"decision", *auto_decision},
      };
      if (*auto_decision == "approved_for_session") {
        if (auto inferred = BuildPermissionToolIdentifierLocal(tool_name, input); inferred.has_value()) {
          completed["allowTools"] = nlohmann::json::array({*inferred});
          session.permission_allow_tools.insert(*inferred);
        }
      }
      session.agent_state_completed_requests[request_id] = std::move(completed);
      session.updated_at_ms = std::max(session.updated_at_ms, now);
      manager->PushEventLocked(session.ns,
                               {{"type", "session-updated"},
                                {"namespace", session.ns},
                                {"sessionId", session.id},
                                {"data", manager->BuildSessionJsonLocked(session)}},
                               session.id);
      manager->PersistStateLocked();
      return nlohmann::json{{"decision", MapRuntimeDecisionToAppServerDecision(*auto_decision)}};
    }

    if (!session.agent_state_requests.is_object()) {
      session.agent_state_requests = nlohmann::json::object();
    }
    if (!session.agent_state_completed_requests.is_object()) {
      session.agent_state_completed_requests = nlohmann::json::object();
    }
    session.agent_state_requests[request_id] = {
        {"tool", tool_name},
        {"arguments", input},
        {"createdAt", now},
    };
    session.agent_state_completed_requests.erase(request_id);
    session.updated_at_ms = std::max(session.updated_at_ms, now);
    session.thinking = false;

    pending = std::make_shared<PendingPermission>();
    pending->tool_name = tool_name;
    pending->input = input;
    {
      std::lock_guard<std::mutex> lock(mu);
      pending_permissions[request_id] = pending;
    }

    const std::string short_title = "Permission Required";
    const std::string short_body = "The agent is waiting for your approval.";
    manager->PushEventLocked(session.ns,
                             {{"type", "toast"},
                              {"namespace", session.ns},
                              {"data", {{"title", short_title},
                                         {"body", short_body},
                                         {"sessionId", session.id},
                                         {"url", "/sessions/" + session.id}}}},
                             session.id);
    manager->PushEventLocked(session.ns,
                             {{"type", "session-updated"},
                              {"namespace", session.ns},
                              {"sessionId", session.id},
                              {"data", manager->BuildSessionJsonLocked(session)}},
                             session.id);
    manager->PersistStateLocked();
  }

  std::unique_lock<std::mutex> pending_lock(pending->mu);
  pending->cv.wait(pending_lock, [&]() { return pending->resolved; });
  const std::string response_decision = pending->response_decision.empty() ? "cancel" : pending->response_decision;
  return nlohmann::json{{"decision", response_decision}};
}

void CodeAgentManager::SessionRunnerState::ReaderLoop() {
#if !defined(__unix__) && !defined(__APPLE__)
  return;
#else
  FILE* stream = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (stdout_fd >= 0) {
      stream = ::fdopen(stdout_fd, "r");
      stdout_fd = -1;
    }
  }
  if (stream == nullptr) {
    FailTurn("failed to open codex app-server stdout");
    return;
  }

  std::array<char, 8192> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), stream) != nullptr) {
    std::string line = util::Trim(std::string(buffer.data()));
    if (line.empty()) {
      continue;
    }

    nlohmann::json payload;
    try {
      payload = nlohmann::json::parse(line);
    } catch (...) {
      continue;
    }
    if (!payload.is_object()) {
      continue;
    }

    auto method_it = payload.find("method");
    if (method_it != payload.end() && method_it->is_string()) {
      const std::string method = method_it->get<std::string>();
      const nlohmann::json params = payload.value("params", nlohmann::json::object());
      auto id_it = payload.find("id");
      if (id_it != payload.end()) {
        const nlohmann::json response = HandleIncomingRequest(method, params);
        WriteJsonLine(nlohmann::json{{"id", *id_it}, {"result", response}});
      } else {
        HandleNotification(method, params);
      }
      continue;
    }

    if (payload.contains("id")) {
      HandleResponse(payload);
    }
  }

  std::fclose(stream);
  {
    std::lock_guard<std::mutex> lock(mu);
    thread_id.clear();
    current_turn_id.clear();
  }
  FailTurn("codex app-server disconnected");
#endif
}

void CodeAgentManager::SessionRunnerState::WorkerLoop() {
  while (true) {
    TurnTask task;
    {
      std::unique_lock<std::mutex> lock(mu);
      cv.wait(lock, [&]() { return stop || !queue.empty(); });
      if (stop) {
        return;
      }
      task = std::move(queue.front());
      queue.pop_front();
      turn_active = true;
      turn_done = false;
      active_generation = task.generation;
      current_turn_id.clear();
      summary_hint.clear();
      parse_state = parser::AgentOutputParseState{};
      event_converter = AppServerEventConverterState{};
    }

    bool fresh_thread = false;
    std::string error;
    if (!EnsureThreadStarted(task, &fresh_thread, &error)) {
      FailTurn(error.empty() ? std::string("failed to start codex thread") : error);
      continue;
    }

    SessionRecord session_snapshot;
    std::string input_text;
    {
      std::lock_guard<std::mutex> manager_lock(manager->mu_);
      auto session_it = manager->sessions_by_id_.find(session_id);
      if (session_it == manager->sessions_by_id_.end()) {
        FailTurn("session not found");
        continue;
      }
      session_snapshot = session_it->second;
      if (fresh_thread || !task.continuation_context.empty()) {
        input_text = manager->BuildConversationPrompt(session_snapshot, task.prompt, task.continuation_context);
      } else {
        input_text = FormatCodexTurnText(task.prompt, task.attachments);
      }
    }

    nlohmann::json turn_start_params;
    {
      std::lock_guard<std::mutex> lock(mu);
      turn_start_params = BuildTurnStartParamsLocked(session_snapshot, input_text);
    }

    nlohmann::json result;
    if (!SendRequest("turn/start", turn_start_params, &result, 30000, &error)) {
      FailTurn(error.empty() ? std::string("failed to start codex turn") : error);
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(mu);
      const nlohmann::json turn = result.value("turn", nlohmann::json::object());
      current_turn_id = JsonStringValue(turn, {"id"}).value_or("");
    }

    std::unique_lock<std::mutex> lock(mu);
    cv.wait(lock, [&]() { return stop || turn_done; });
    if (stop) {
      return;
    }
  }
}

std::shared_ptr<CodeAgentManager::SessionRunnerState> CodeAgentManager::EnsureSessionRunnerLocked(SessionRecord& session,
                                                                                                  std::string* error) {
  if (session.flavor != "codex") {
    return nullptr;
  }
  auto existing = session_runners_.find(session.id);
  if (existing != session_runners_.end()) {
    return existing->second;
  }

  const auto runner = std::make_shared<SessionRunnerState>(this, session);
  if (!runner->Start(error)) {
    return nullptr;
  }
  session_runners_[session.id] = runner;
  return runner;
}

void CodeAgentManager::StopSessionRunner(const std::shared_ptr<SessionRunnerState>& runner) {
  if (!runner) {
    return;
  }
  runner->Stop();
}

void CodeAgentManager::StopSessionRunnerLocked(const std::string& session_id) {
  session_runners_.erase(session_id);
}

bool CodeAgentManager::InterruptSessionRunnerLocked(const std::string& session_id, std::string* error) {
  auto it = session_runners_.find(session_id);
  if (it == session_runners_.end() || !it->second) {
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }
  return it->second->Interrupt(error);
}

bool CodeAgentManager::StartSessionTurn(const std::string& session_id, std::uint64_t generation,
                                        const std::string& prompt, const nlohmann::json& attachments,
                                        const std::string& continuation_context, std::string* error) {
  std::shared_ptr<SessionRunnerState> runner;
  bool requires_live_runner = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto session_it = sessions_by_id_.find(session_id);
    if (session_it == sessions_by_id_.end()) {
      if (error != nullptr) {
        *error = "session not found";
      }
      return false;
    }
    requires_live_runner = session_it->second.flavor == "codex";
    runner = EnsureSessionRunnerLocked(session_it->second, error);
    if (requires_live_runner && runner == nullptr) {
      return false;
    }
  }

  if (runner != nullptr) {
    return runner->EnqueueTurn(generation, prompt, attachments, continuation_context, error);
  }

  StartAgentRun(session_id, generation, prompt, continuation_context);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool CodeAgentManager::AutoResolveSessionRunnerPermissionsLocked(SessionRecord& session,
                                                                 const std::string& normalized_mode,
                                                                 std::vector<std::string>* resolved_lines) {
  if (resolved_lines != nullptr) {
    resolved_lines->clear();
  }

  auto runner_it = session_runners_.find(session.id);
  if (runner_it == session_runners_.end() || !runner_it->second) {
    return false;
  }
  auto runner = runner_it->second;

  struct ResolvedPermission {
    std::string request_id;
    std::string tool_name;
    nlohmann::json input = nlohmann::json::object();
    std::string decision;
    std::vector<std::string> allow_tools;
    std::shared_ptr<SessionRunnerState::PendingPermission> pending;
  };

  std::vector<ResolvedPermission> resolved;
  {
    std::lock_guard<std::mutex> runner_lock(runner->mu);
    for (auto pending_it = runner->pending_permissions.begin(); pending_it != runner->pending_permissions.end();) {
      const std::string request_id = pending_it->first;
      const auto pending = pending_it->second;
      const std::string tool_name = pending ? pending->tool_name : std::string("Tool");
      const nlohmann::json input = pending ? pending->input : nlohmann::json::object();

      auto decision = ResolveAutoPermissionDecisionLocal(session.permission_allow_tools, normalized_mode, tool_name, input);
      if (!decision.has_value()) {
        ++pending_it;
        continue;
      }

      ResolvedPermission item;
      item.request_id = request_id;
      item.tool_name = tool_name;
      item.input = input;
      item.decision = *decision;
      item.pending = pending;
      if (*decision == "approved_for_session") {
        if (auto inferred = BuildPermissionToolIdentifierLocal(tool_name, input); inferred.has_value()) {
          item.allow_tools.push_back(*inferred);
        }
      }

      resolved.push_back(std::move(item));
      pending_it = runner->pending_permissions.erase(pending_it);
    }
  }

  if (resolved.empty()) {
    return false;
  }

  if (!session.agent_state_requests.is_object()) {
    session.agent_state_requests = nlohmann::json::object();
  }
  if (!session.agent_state_completed_requests.is_object()) {
    session.agent_state_completed_requests = nlohmann::json::object();
  }

  const std::int64_t now = NowMs();
  for (auto& item : resolved) {
    std::int64_t created_at = now;
    auto request_it = session.agent_state_requests.find(item.request_id);
    if (request_it != session.agent_state_requests.end() && request_it->is_object()) {
      created_at = request_it->value("createdAt", now);
      session.agent_state_requests.erase(request_it);
    } else {
      session.agent_state_requests.erase(item.request_id);
    }
    if (!item.allow_tools.empty()) {
      MergePermissionAllowToolsLocal(&session.permission_allow_tools, item.allow_tools);
    }

    nlohmann::json completed = {
        {"tool", item.tool_name},
        {"arguments", item.input},
        {"createdAt", created_at},
        {"completedAt", now},
        {"status", item.decision == "denied" ? "denied" : "approved"},
        {"decision", item.decision},
    };
    if (normalized_mode != "default") {
      completed["mode"] = normalized_mode;
    }
    if (!item.allow_tools.empty()) {
      completed["allowTools"] = item.allow_tools;
    }
    session.agent_state_completed_requests[item.request_id] = std::move(completed);
    session.updated_at_ms = std::max(session.updated_at_ms, now);

    if (resolved_lines != nullptr) {
      resolved_lines->push_back("- " + item.tool_name + " (" + item.decision + ")");
    }
  }

  {
    std::lock_guard<std::mutex> runner_lock(runner->mu);
    if (runner->turn_active && session.agent_state_requests.empty()) {
      session.thinking = true;
    }
  }

  for (auto& item : resolved) {
    if (!item.pending) {
      continue;
    }
    std::lock_guard<std::mutex> pending_lock(item.pending->mu);
    item.pending->resolved = true;
    item.pending->response_decision = MapRuntimeDecisionToAppServerDecision(item.decision);
    item.pending->cv.notify_all();
  }

  return true;
}

bool CodeAgentManager::ResolveSessionRunnerPermissionLocked(SessionRecord& session, const std::string& request_id,
                                                            bool approved, const std::string& mode,
                                                            const std::vector<std::string>& allow_tools,
                                                            const std::string& decision,
                                                            const nlohmann::json& answers, std::string* error,
                                                            bool* handled) {
  if (handled != nullptr) {
    *handled = false;
  }
  auto runner_it = session_runners_.find(session.id);
  if (runner_it == session_runners_.end() || !runner_it->second) {
    return false;
  }
  auto runner = runner_it->second;

  std::shared_ptr<SessionRunnerState::PendingPermission> pending;
  {
    std::lock_guard<std::mutex> runner_lock(runner->mu);
    auto pending_it = runner->pending_permissions.find(request_id);
    if (pending_it == runner->pending_permissions.end()) {
      return false;
    }
    pending = pending_it->second;
  }

  if (handled != nullptr) {
    *handled = true;
  }

  if (!session.agent_state_requests.is_object()) {
    session.agent_state_requests = nlohmann::json::object();
  }
  auto request_it = session.agent_state_requests.find(request_id);
  if (request_it == session.agent_state_requests.end() || !request_it->is_object()) {
    if (error != nullptr) {
      *error = "request not found";
    }
    return false;
  }

  const std::string trimmed_mode = util::Trim(mode);
  std::string normalized_mode =
      trimmed_mode.empty() ? std::string() : policy::CanonicalizePermissionModeForFlavor(trimmed_mode, session.flavor);
  if (!trimmed_mode.empty() && normalized_mode.empty()) {
    if (error != nullptr) {
      *error = "invalid permission mode for session flavor";
    }
    return false;
  }

  std::string normalized_decision = ToLowerCopy(util::Trim(decision));
  if (!normalized_decision.empty() && normalized_decision != "approved" && normalized_decision != "approved_for_session" &&
      normalized_decision != "denied" && normalized_decision != "abort") {
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
    if (!normalized_tool.empty() && allow_tool_dedup.insert(normalized_tool).second) {
      effective_allow_tools.push_back(normalized_tool);
    }
  }

  {
    std::lock_guard<std::mutex> runner_lock(runner->mu);
    auto pending_it = runner->pending_permissions.find(request_id);
    if (pending_it == runner->pending_permissions.end()) {
      if (error != nullptr) {
        *error = "request not found";
      }
      return false;
    }
    runner->pending_permissions.erase(pending_it);
  }

  const std::int64_t now = NowMs();
  const nlohmann::json request = *request_it;
  const std::string request_tool = request.value("tool", std::string("Tool"));
  const nlohmann::json request_arguments = request.value("arguments", nlohmann::json::object());
  session.agent_state_requests.erase(request_it);
  if (!session.agent_state_completed_requests.is_object()) {
    session.agent_state_completed_requests = nlohmann::json::object();
  }

  if (approved && normalized_decision == "approved_for_session" && effective_allow_tools.empty()) {
    if (auto inferred = BuildPermissionToolIdentifierLocal(request_tool, request_arguments); inferred.has_value()) {
      effective_allow_tools.push_back(*inferred);
    }
  }
  if (!effective_allow_tools.empty()) {
    MergePermissionAllowToolsLocal(&session.permission_allow_tools, effective_allow_tools);
  }

  nlohmann::json completed = {
      {"tool", request_tool},
      {"arguments", request_arguments},
      {"createdAt", request.value("createdAt", now)},
      {"completedAt", now},
      {"status", approved ? "approved" : "denied"},
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
  if (!approved) {
    completed["reason"] = normalized_decision == "abort" ? "Aborted by user" : "Denied by user";
  }

  if (!normalized_mode.empty()) {
    const bool mode_changed = session.permission_mode != normalized_mode;
    ApplySessionRuntimeConfigLocked(session, normalized_mode);
    if (mode_changed) {
      EmitAgentEventMessageLocked(session,
                                  {{"type", "permission-mode-changed"}, {"mode", normalized_mode}});
    }
  }

  session.agent_state_completed_requests[request_id] = std::move(completed);
  session.updated_at_ms = now;
  {
    std::lock_guard<std::mutex> runner_lock(runner->mu);
    if (runner->turn_active && session.agent_state_requests.empty()) {
      session.thinking = true;
    }
  }

  {
    std::lock_guard<std::mutex> pending_lock(pending->mu);
    pending->resolved = true;
    pending->response_decision = approved
                                     ? MapRuntimeDecisionToAppServerDecision(normalized_decision.empty() ? std::string("approved")
                                                                                                         : normalized_decision)
                                     : MapRuntimeDecisionToAppServerDecision(normalized_decision.empty() ? std::string("denied")
                                                                                                         : normalized_decision);
    pending->cv.notify_all();
  }

  PushEventLocked(session.ns,
                  {{"type", "session-updated"},
                   {"namespace", session.ns},
                   {"sessionId", session.id},
                   {"data", BuildSessionJsonLocked(session)}},
                  session.id);
  PersistStateLocked();
  if (error != nullptr) {
    error->clear();
  }
  return true;
}


void CodeAgentManager::StartAgentRun(const std::string& session_id, std::uint64_t generation, std::string prompt,
                                     std::string continuation_context) {
  std::thread([this, session_id, generation, prompt = std::move(prompt),
               continuation_context = std::move(continuation_context)]() {
    SessionRecord snapshot;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto session_it = sessions_by_id_.find(session_id);
      if (session_it == sessions_by_id_.end()) {
        return;
      }
      if (session_it->second.generation != generation) {
        return;
      }
      snapshot = session_it->second;
    }

    parser::AgentOutputParseState parse_state;
    const std::string plain_message_id = "msg-" + util::RandomHex(18);
    std::string line_buffer;
    std::string plain_output;
    std::string summary_hint;
    bool emitted_plain = false;
    bool emitted_structured = false;

    const auto emit_structured_bodies = [this, &session_id, generation, &emitted_structured, &summary_hint](
                                            const std::vector<nlohmann::json>& bodies) {
      for (const auto& body : bodies) {
        std::string body_summary;
        if (EmitCodexBodyMessage(session_id, generation, body, &body_summary)) {
          emitted_structured = true;
          if (!util::Trim(body_summary).empty()) {
            summary_hint = body_summary;
          }
        }
      }
    };

    const auto emit_plain_text = [this, &session_id, generation, &plain_message_id, &plain_output, &summary_hint,
                                  &emitted_plain](std::string_view text) {
      if (text.empty()) {
        return;
      }
      if (!AppendAssistantStreamChunk(session_id, generation, plain_message_id, text)) {
        return;
      }
      emitted_plain = true;
      plain_output.append(text.data(), text.size());
      const std::string text_copy(text);
      if (!util::Trim(text_copy).empty()) {
        summary_hint = text_copy;
      }
    };

    const auto process_output_line = [&parse_state, &emit_structured_bodies, &emit_plain_text](const std::string& line) {
      const std::string trimmed = util::Trim(line);
      if (!trimmed.empty() && (trimmed.front() == '{' || trimmed.front() == '[')) {
        try {
          nlohmann::json parsed = nlohmann::json::parse(trimmed);
          std::vector<nlohmann::json> bodies;
          if (parser::ParseAgentOutputJson(parsed, &parse_state, &bodies)) {
            emit_structured_bodies(bodies);
            return;
          }
        } catch (...) {
        }
      }
      emit_plain_text(line);
    };

    int exit_code = -1;
    std::string output = ExecuteAgentCommand(
        snapshot, prompt, continuation_context, &exit_code,
        [&line_buffer, &process_output_line](std::string_view chunk) {
          if (chunk.empty()) {
            return;
          }
          line_buffer.append(chunk.data(), chunk.size());
          while (true) {
            const size_t newline = line_buffer.find('\n');
            if (newline == std::string::npos) {
              break;
            }
            const std::string line = line_buffer.substr(0, newline + 1);
            line_buffer.erase(0, newline + 1);
            process_output_line(line);
          }
        });

    if (!line_buffer.empty()) {
      process_output_line(line_buffer);
      line_buffer.clear();
    }

    std::vector<nlohmann::json> buffered_bodies;
    parser::DrainBufferedBodies(&parse_state, &buffered_bodies);
    emit_structured_bodies(buffered_bodies);

    if (exit_code != 0) {
      const std::string notice = "[codeagent] process exited with code " + std::to_string(exit_code);
      if (emitted_plain || !emitted_structured) {
        if (emitted_plain) {
          emit_plain_text("\n\n");
        }
        emit_plain_text(notice);
      } else {
        emit_structured_bodies({parser::CodexMessageBody(notice)});
      }
    }

    if (!emitted_plain && !emitted_structured) {
      emit_plain_text("(empty output)");
    }

    std::lock_guard<std::mutex> lock(mu_);
    auto session_it = sessions_by_id_.find(session_id);
    if (session_it == sessions_by_id_.end()) {
      return;
    }

    SessionRecord& session = session_it->second;
    if (session.generation != generation) {
      return;
    }

    std::optional<MessageRecord> plain_snapshot;
    if (emitted_plain) {
      auto message_it = std::find_if(session.messages.begin(), session.messages.end(),
                                     [&plain_message_id](const MessageRecord& message) {
                                       return message.id == plain_message_id;
                                     });
      if (message_it != session.messages.end()) {
        message_it->content = {
            {"role", "agent"},
            {"content", plain_output},
            {"meta", {{"sentFrom", "cli"}}},
        };
        plain_snapshot = *message_it;
      } else {
        MessageRecord assistant_message;
        assistant_message.id = plain_message_id;
        assistant_message.seq = static_cast<int>(session.messages.size() + 1);
        assistant_message.local_id.clear();
        assistant_message.created_at_ms = NowMs();
        assistant_message.content = {
            {"role", "agent"},
            {"content", plain_output},
            {"meta", {{"sentFrom", "cli"}}},
        };
        session.messages.push_back(assistant_message);
        plain_snapshot = assistant_message;
      }
    }

    if (plain_snapshot.has_value()) {
      PublishMessageReceivedLocked(session, *plain_snapshot);
    }

    std::string summary_source;
    if (!util::Trim(plain_output).empty()) {
      summary_source = plain_output;
    } else if (!util::Trim(summary_hint).empty()) {
      summary_source = summary_hint;
    } else if (!util::Trim(output).empty()) {
      summary_source = output;
    } else {
      summary_source = "(empty output)";
    }

    session.summary_text = FirstLine(summary_source);
    session.summary_updated_at_ms = NowMs();
    session.updated_at_ms = session.summary_updated_at_ms;
    session.thinking = false;

    PushEventLocked(session.ns,
                    {
                        {"type", "session-updated"},
                        {"namespace", session.ns},
                        {"sessionId", session.id},
                        {"data", BuildSessionJsonLocked(session)},
                    },
                    session.id);
    PersistStateLocked();
  }).detach();
}

bool CodeAgentManager::AppendAssistantStreamChunk(const std::string& session_id, std::uint64_t generation,
                                                  const std::string& message_id, std::string_view chunk) {
  if (chunk.empty()) {
    return false;
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

  auto message_it = std::find_if(session.messages.begin(), session.messages.end(),
                                 [&message_id](const MessageRecord& message) {
                                   return message.id == message_id;
                                 });
  if (message_it == session.messages.end()) {
    MessageRecord assistant_message;
    assistant_message.id = message_id;
    assistant_message.seq = static_cast<int>(session.messages.size() + 1);
    assistant_message.local_id.clear();
    assistant_message.created_at_ms = NowMs();
    assistant_message.content = {
        {"role", "agent"},
        {"content", std::string()},
        {"meta", {{"sentFrom", "cli"}}},
    };
    session.messages.push_back(std::move(assistant_message));
    message_it = std::prev(session.messages.end());
  }

  if (!message_it->content.is_object()) {
    message_it->content = {
        {"role", "agent"},
        {"content", std::string()},
        {"meta", {{"sentFrom", "cli"}}},
    };
  }
  message_it->content["role"] = "agent";
  message_it->content["meta"] = nlohmann::json{{"sentFrom", "cli"}};
  if (!message_it->content.contains("content") || !message_it->content["content"].is_string()) {
    message_it->content["content"] = std::string();
  }
  std::string& content = message_it->content["content"].get_ref<std::string&>();
  content.append(chunk.data(), chunk.size());

  session.updated_at_ms = NowMs();
  MessageRecord snapshot = *message_it;
  PublishMessageReceivedLocked(session, snapshot);
  return true;
}

std::string CodeAgentManager::ShellEscape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('\'');
  for (char c : value) {
    if (c == '\'') {
      escaped += "'\\''";
    } else {
      escaped.push_back(c);
    }
  }
  escaped.push_back('\'');
  return escaped;
}

std::string CodeAgentManager::BuildConversationPrompt(const SessionRecord& session, const std::string& prompt,
                                                      const std::string& continuation_context) const {
  auto json_string = [](const nlohmann::json& value, std::initializer_list<const char*> keys) -> std::optional<std::string> {
    if (!value.is_object()) {
      return std::nullopt;
    }
    for (const char* key : keys) {
      auto it = value.find(key);
      if (it != value.end() && it->is_string()) {
        const std::string text = util::Trim(it->get<std::string>());
        if (!text.empty()) {
          return text;
        }
      }
    }
    return std::nullopt;
  };

  auto json_field = [](const nlohmann::json& value, std::initializer_list<const char*> keys) -> const nlohmann::json* {
    if (!value.is_object()) {
      return nullptr;
    }
    for (const char* key : keys) {
      auto it = value.find(key);
      if (it != value.end()) {
        return &(*it);
      }
    }
    return nullptr;
  };

  auto truncate = [](std::string text, size_t max_chars) {
    text = util::Trim(text);
    if (text.size() <= max_chars) {
      return text;
    }
    return text.substr(0, max_chars) + "...";
  };

  auto safe_dump = [&truncate](const nlohmann::json& value, size_t max_chars) {
    return truncate(value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), max_chars);
  };

  auto attachment_suffix = [&json_string](const nlohmann::json& content) {
    if (!content.is_object()) {
      return std::string();
    }
    auto attachments_it = content.find("attachments");
    if (attachments_it == content.end() || !attachments_it->is_array() || attachments_it->empty()) {
      return std::string();
    }

    std::vector<std::string> names;
    names.reserve(attachments_it->size());
    for (const auto& attachment : *attachments_it) {
      if (attachment.is_string()) {
        const std::string name = util::Trim(attachment.get<std::string>());
        if (!name.empty()) {
          names.push_back(name);
        }
        continue;
      }
      if (!attachment.is_object()) {
        continue;
      }
      auto name = json_string(attachment, {"name", "filename", "path", "url"});
      if (name.has_value()) {
        names.push_back(*name);
      }
    }

    if (names.empty()) {
      return std::string(" [attachments]");
    }

    constexpr size_t kMaxAttachmentNames = 3;
    std::string suffix = " [attachments: ";
    for (size_t index = 0; index < names.size() && index < kMaxAttachmentNames; ++index) {
      if (index > 0) {
        suffix += ", ";
      }
      suffix += names[index];
    }
    if (names.size() > kMaxAttachmentNames) {
      suffix += ", ...";
    }
    suffix += "]";
    return suffix;
  };

  auto serialize_codex_data = [&json_string, &json_field, &safe_dump, &truncate](const nlohmann::json& data) {
    const std::string type = json_string(data, {"type"}).value_or("");
    if (type == "message") {
      return std::string("Assistant: ") + truncate(json_string(data, {"message", "text"}).value_or(""), 1200);
    }
    if (type == "tool-call") {
      const std::string name = json_string(data, {"name", "tool", "toolName", "tool_name"}).value_or("Tool");
      if (const nlohmann::json* input = json_field(data, {"input", "arguments"}); input != nullptr && !input->is_null()) {
        return std::string("Assistant tool call ") + name + ": " + safe_dump(*input, 900);
      }
      return std::string("Assistant tool call ") + name;
    }
    if (type == "tool-call-result") {
      const std::string name = json_string(data, {"name", "tool", "toolName", "tool_name"}).value_or("tool");
      if (const nlohmann::json* output = json_field(data, {"output", "result"}); output != nullptr && !output->is_null()) {
        return std::string("Assistant tool result ") + name + ": " + safe_dump(*output, 900);
      }
      return std::string("Assistant tool result ") + name;
    }
    if (type == "reasoning" || type == "token-count") {
      return std::string();
    }
    return std::string();
  };

  auto serialize_message = [&](const nlohmann::json& message) {
    if (!message.is_object()) {
      return std::string();
    }

    const std::string role = json_string(message, {"role"}).value_or("");
    const nlohmann::json* content = json_field(message, {"content"});
    if (content == nullptr) {
      return std::string();
    }

    if (role == "user") {
      if (content->is_string()) {
        return std::string("User: ") + truncate(content->get<std::string>(), 1200);
      }
      if (!content->is_object()) {
        return std::string();
      }

      const std::string type = json_string(*content, {"type"}).value_or("");
      if (type == "text" || type.empty()) {
        const std::string text = json_string(*content, {"text", "message"}).value_or("");
        const std::string suffix = attachment_suffix(*content);
        if (text.empty()) {
          return suffix.empty() ? std::string() : std::string("User: (sent attachments)") + suffix;
        }
        return std::string("User: ") + truncate(text, 1200) + suffix;
      }

      return std::string("User: ") + safe_dump(*content, 900);
    }

    if (role != "agent") {
      return std::string();
    }

    if (content->is_string()) {
      return std::string("Assistant: ") + truncate(content->get<std::string>(), 1200);
    }
    if (!content->is_object()) {
      return std::string();
    }

    const std::string type = json_string(*content, {"type"}).value_or("");
    if (type == "event") {
      return std::string();
    }
    if (type == "codex") {
      if (const nlohmann::json* data = json_field(*content, {"data"}); data != nullptr && data->is_object()) {
        return serialize_codex_data(*data);
      }
      return std::string();
    }

    if (type == "text") {
      return std::string("Assistant: ") + truncate(json_string(*content, {"text", "message"}).value_or(""), 1200);
    }

    return std::string();
  };

  const std::string trimmed_prompt = util::Trim(prompt);
  const std::string trimmed_continuation = util::Trim(continuation_context);
  const bool has_internal_update = !trimmed_continuation.empty();

  std::string latest_entry;
  if (!session.messages.empty()) {
    latest_entry = serialize_message(session.messages.back().content);
  }
  if (latest_entry.empty()) {
    latest_entry = trimmed_prompt.empty() ? std::string("User: (empty message)") : std::string("User: ") + trimmed_prompt;
  }

  if (!trimmed_prompt.empty() && trimmed_prompt.front() == '/') {
    return prompt;
  }

  if (!has_internal_update && session.messages.size() <= 1) {
    return trimmed_prompt.empty() ? latest_entry : prompt;
  }

  constexpr size_t kMaxHistoryEntries = 48;
  constexpr size_t kMaxHistoryChars = 24000;
  std::vector<std::string> history_entries;
  history_entries.reserve(std::min(kMaxHistoryEntries, session.messages.size() - 1));

  size_t consumed_chars = 0;
  for (size_t index = session.messages.size() - 1; index > 0; --index) {
    const std::string entry = serialize_message(session.messages[index - 1].content);
    if (entry.empty()) {
      continue;
    }

    const size_t next_chars = entry.size() + 2;
    if (!history_entries.empty() && consumed_chars + next_chars > kMaxHistoryChars) {
      break;
    }

    history_entries.push_back(entry);
    consumed_chars += next_chars;
    if (history_entries.size() >= kMaxHistoryEntries) {
      break;
    }
  }

  if (!has_internal_update && history_entries.empty()) {
    return trimmed_prompt.empty() ? latest_entry : prompt;
  }

  std::reverse(history_entries.begin(), history_entries.end());

  std::string combined;
  combined.reserve(consumed_chars + latest_entry.size() + 512);
  combined += "You are continuing the same Ferryman session. Use the earlier conversation below as context. ";
  combined += "Do not say that the prior context is missing when it is present in the transcript.\n\n";
  if (session.messages.size() - 1 > history_entries.size()) {
    combined += "[Earlier conversation omitted for brevity]\n\n";
  }
  combined += "Conversation so far:\n";
  for (const auto& entry : history_entries) {
    combined += entry;
    combined += "\n\n";
  }
  if (has_internal_update) {
    combined += "Latest visible conversation entry:\n";
    combined += latest_entry;
    combined += "\n\nInternal session update (not shown to the user):\n";
    combined += trimmed_continuation;
    combined += "\n\nContinue the same conversation naturally. Treat the internal session update as already-applied tool approval or user input. Do not claim the user sent that internal update as a chat message, and do not ask them to repeat it.";
  } else {
    combined += "Latest user message:\n";
    combined += latest_entry;
    combined += "\n\nContinue the same conversation naturally and answer the latest user message.";
  }
  return combined;
}

std::string CodeAgentManager::BuildAgentCommand(const SessionRecord& session, const std::string& prompt,
                                                const std::string& continuation_context) const {
  std::string cmd_template;
  if (session.flavor == "codex") {
    cmd_template = codex_cmd_template_;
  } else if (session.flavor == "cursor") {
    cmd_template = cursor_cmd_template_;
  } else if (session.flavor == "gemini") {
    cmd_template = gemini_cmd_template_;
  } else if (session.flavor == "opencode") {
    cmd_template = opencode_cmd_template_;
  } else {
    cmd_template = claude_cmd_template_;
  }

  const std::string effective_prompt = BuildConversationPrompt(session, prompt, continuation_context);
  const std::string escaped_prompt = ShellEscape(effective_prompt);
  const std::string escaped_cwd = ShellEscape(session.path.string());
  std::string escaped_model = ShellEscape(session.model);

  ReplaceAll(&cmd_template, "{prompt}", escaped_prompt);
  ReplaceAll(&cmd_template, "{cwd}", escaped_cwd);
  ReplaceAll(&cmd_template, "{model}", escaped_model);

  if (cmd_template.find("{prompt}") != std::string::npos) {
    cmd_template += " " + escaped_prompt;
  }

  if (cmd_template.find("cd ") == std::string::npos && cmd_template.find("{cwd}") == std::string::npos) {
    cmd_template = "cd " + escaped_cwd + " && " + cmd_template;
  }

  std::string permission_mode = util::Trim(session.permission_mode);
  permission_mode = policy::CanonicalizePermissionModeForFlavor(permission_mode, session.flavor);
  if (permission_mode.empty()) {
    permission_mode = policy::DefaultPermissionModeForFlavor(session.flavor);
  }

  if (session.flavor == "codex") {
    const CodexPermissionConfig config = ResolveCodexPermissionConfig(permission_mode);
    AppendCodexCommandOption(&cmd_template, "--ask-for-approval", config.approval_policy,
                             config.allow_default_override);
    AppendCodexCommandOption(&cmd_template, "--sandbox", config.sandbox_mode, config.allow_default_override);
    if (!session.model_reasoning_effort.empty() && policy::IsKnownReasoningEffort(session.model_reasoning_effort) &&
        !HasCodexConfigKey(cmd_template, "model_reasoning_effort")) {
      const std::string config_override = "model_reasoning_effort=\"" + session.model_reasoning_effort + "\"";
      AppendCodexCommandOption(&cmd_template, "--config", ShellEscape(config_override));
    }

    if (!HasCodexConfigKey(cmd_template, "developer_instructions")) {
      const std::string instruction_override =
          "developer_instructions=\"" + EscapeTomlString(CodexTitleDeveloperInstruction()) + "\"";
      AppendCodexCommandOption(&cmd_template, "--config", ShellEscape(instruction_override));
    }

    const bool has_hapi_mcp_command = HasCodexConfigKey(cmd_template, "mcp_servers.hapi.command");
    const bool has_hapi_mcp_args = HasCodexConfigKey(cmd_template, "mcp_servers.hapi.args");
    if (!has_hapi_mcp_command || !has_hapi_mcp_args) {
      const auto title_mcp_script = ResolveCodexTitleMcpScriptPath(state_file_path_);
      if (EnsureCodexTitleMcpScript(title_mcp_script)) {
        if (!has_hapi_mcp_command) {
          const std::string mcp_command = EnvOrDefault("FERRYMAN_CODEAGENT_CODEX_TITLE_MCP_COMMAND", "python3");
          const std::string command_override =
              "mcp_servers.hapi.command=\"" + EscapeTomlString(mcp_command) + "\"";
          AppendCodexCommandOption(&cmd_template, "--config", ShellEscape(command_override));
        }
        if (!has_hapi_mcp_args) {
          const std::string args_override =
              "mcp_servers.hapi.args=" +
              BuildTomlLiteralArray(std::vector<std::string>{"-u", title_mcp_script.string()});
          AppendCodexCommandOption(&cmd_template, "--config", ShellEscape(args_override));
        }
      }
    }
  } else if (session.flavor == "claude") {
    const bool allow_default_override = permission_mode == "default";
    AppendCommandOption(&cmd_template, "--permission-mode", permission_mode, allow_default_override);
    const bool uses_stream_json = cmd_template.find("--output-format stream-json") != std::string::npos ||
                                  cmd_template.find("--output-format=stream-json") != std::string::npos;
    if (uses_stream_json && !HasCommandOption(cmd_template, "--include-partial-messages")) {
      AppendCommandOption(&cmd_template, "--include-partial-messages", "");
    }

    if (!session.title_initialized) {
      if (!HasCommandOption(cmd_template, "--append-system-prompt")) {
        AppendCommandOption(&cmd_template, "--append-system-prompt", ShellEscape(ClaudeTitleSystemPrompt()));
      }

      if (!HasCommandOption(cmd_template, "--mcp-config")) {
        const auto title_mcp_script = ResolveCodexTitleMcpScriptPath(state_file_path_);
        if (EnsureCodexTitleMcpScript(title_mcp_script)) {
          const std::string mcp_command = EnvOrDefault("FERRYMAN_CODEAGENT_CLAUDE_TITLE_MCP_COMMAND", "python3");
          nlohmann::json mcp_config = {
              {"mcpServers", {
                                 {"hapi", {{"command", mcp_command},
                                            {"args", nlohmann::json::array({"-u", title_mcp_script.string()})}}},
                             }},
          };
          AppendCommandOption(&cmd_template, "--mcp-config", ShellEscape(mcp_config.dump()));
        }
      }
    }
  } else if (session.flavor == "cursor") {
    if (permission_mode == "plan" || permission_mode == "ask") {
      AppendCommandOption(&cmd_template, "--mode", permission_mode);
    } else if (permission_mode == "force") {
      AppendCommandOption(&cmd_template, "--force", "");
    }
  } else if (session.flavor == "gemini") {
    const std::string approval_mode = ResolveGeminiApprovalMode(permission_mode);
    AppendCommandOption(&cmd_template, "--approval-mode", approval_mode);
  }


  return cmd_template;
}

std::string CodeAgentManager::ReadCommandOutput(const std::string& command, int* exit_code,
                                                const std::function<void(std::string_view)>& on_chunk) {
#if defined(_WIN32)
  FILE* pipe = ::_popen(command.c_str(), "r");
#else
  FILE* pipe = ::popen(command.c_str(), "r");
#endif
  if (pipe == nullptr) {
    if (exit_code != nullptr) {
      *exit_code = -1;
    }
    return "failed to start agent process";
  }

  std::string output;
  std::array<char, 2048> buffer{};
  while (true) {
    const size_t read_count = std::fread(buffer.data(), 1, buffer.size(), pipe);
    if (read_count > 0) {
      output.append(buffer.data(), read_count);
      if (on_chunk) {
        on_chunk(std::string_view(buffer.data(), read_count));
      }
    }
    if (read_count < buffer.size()) {
      if (std::feof(pipe)) {
        break;
      }
      if (std::ferror(pipe)) {
        break;
      }
    }
  }

#if defined(_WIN32)
  int code = ::_pclose(pipe);
#else
  int code = ::pclose(pipe);
  if (WIFEXITED(code)) {
    code = WEXITSTATUS(code);
  }
#endif
  if (exit_code != nullptr) {
    *exit_code = code;
  }
  return output;
}

std::string CodeAgentManager::ExecuteAgentCommand(const SessionRecord& session, const std::string& prompt,
                                                  const std::string& continuation_context, int* exit_code,
                                                  const std::function<void(std::string_view)>& on_chunk) const {
  const std::string command = BuildAgentCommand(session, prompt, continuation_context) + " 2>&1";
  return ReadCommandOutput(command, exit_code, on_chunk);
}

}  // namespace ferryman::codeagent
