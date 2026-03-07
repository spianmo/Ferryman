#include "CodeAgentOutputParser.hpp"
#include "CodeAgentPolicy.hpp"
#include "ferryman/codeagent/CodeAgentManager.hpp"

#include "ferryman/util/Random.hpp"
#include "ferryman/util/StringUtil.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
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
  const std::string normalized = policy::NormalizePermissionModeValue(permission_mode);
  if (normalized == "read-only") {
    return CodexPermissionConfig{.approval_policy = "never", .sandbox_mode = "read-only"};
  }
  if (normalized == "safe-yolo") {
    return CodexPermissionConfig{.approval_policy = "on-failure", .sandbox_mode = "workspace-write"};
  }
  if (normalized == "yolo") {
    return CodexPermissionConfig{.approval_policy = "on-failure", .sandbox_mode = "danger-full-access"};
  }
  return CodexPermissionConfig{
      .approval_policy = "untrusted", .sandbox_mode = "workspace-write", .allow_default_override = true};
}

std::string ResolveGeminiApprovalMode(const std::string& permission_mode) {
  const std::string normalized = policy::NormalizePermissionModeValue(permission_mode);
  if (normalized == "safe-yolo") {
    return "auto_edit";
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
ALWAYS when you start a new chat, call the title tool exactly once to set a concise task title.
Prefer calling functions.hapi__change_title.
If that exact tool name is unavailable, call an equivalent alias such as hapi__change_title, mcp__hapi__change_title, or hapi_change_title.
Do not call any title-change tool again in this session.
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

  std::string permission_mode = policy::NormalizePermissionModeValue(session.permission_mode);
  if (permission_mode.empty()) {
    permission_mode = "default";
  }
  if (permission_mode == "default" && session.yolo) {
    if (session.flavor == "claude") {
      permission_mode = "bypassPermissions";
    } else {
      permission_mode = "yolo";
    }
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

    if (!session.title_initialized) {
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
    }
  } else if (session.flavor == "claude") {
    const bool allow_default_override = permission_mode == "default";
    AppendCommandOption(&cmd_template, "--permission-mode", permission_mode, allow_default_override);

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
    } else if (permission_mode == "yolo") {
      AppendCommandOption(&cmd_template, "--yolo", "");
    }
  } else if (session.flavor == "gemini") {
    const std::string approval_mode = ResolveGeminiApprovalMode(permission_mode);
    AppendCommandOption(&cmd_template, "--approval-mode", approval_mode);
  } else if (session.yolo) {
    cmd_template += " --yolo";
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
