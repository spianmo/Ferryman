#include "ferryman/codeagent/CodeAgentManager.hpp"

#include "CodeAgentPolicy.hpp"
#include "ferryman/util/StringUtil.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <system_error>
#include <unordered_map>

#include <sqlite3.h>

namespace ferryman::codeagent {
namespace {

#if defined(_WIN32)
#define FERRYMAN_TIMEGM _mkgmtime
#else
#define FERRYMAN_TIMEGM timegm
#endif

struct LocalSessionSummary {
  std::string source_session_id;
  std::filesystem::path transcript_path;
  std::filesystem::path cwd;
  std::string name;
  std::string summary_text;
  std::string model;
  std::string permission_mode;
  std::string reasoning_effort;
  bool codex_fast = false;
  std::int64_t created_at_ms = 0;
  std::int64_t updated_at_ms = 0;
};

std::string GetHomeDir() {
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::string(home);
  }
#if defined(_WIN32)
  const char* home_drive = std::getenv("HOMEDRIVE");
  const char* home_path = std::getenv("HOMEPATH");
  if (home_drive != nullptr && home_path != nullptr && *home_drive != '\0' && *home_path != '\0') {
    return std::string(home_drive) + std::string(home_path);
  }
  if (const char* profile = std::getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
    return std::string(profile);
  }
#endif
  return {};
}

std::string ToLowerCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool LooksLikeExternalSourceId(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (size_t index = 0; index < value.size(); ++index) {
    const char c = value[index];
    const bool hyphen_position = index == 8 || index == 13 || index == 18 || index == 23;
    if (hyphen_position) {
      if (c != '-') {
        return false;
      }
      continue;
    }
    if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

std::int64_t FileTimeToUnixMs(const std::filesystem::file_time_type& value) {
  const auto system_now = std::chrono::system_clock::now();
  const auto file_now = std::filesystem::file_time_type::clock::now();
  const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(value - file_now + system_now);
  return std::chrono::duration_cast<std::chrono::milliseconds>(system_time.time_since_epoch()).count();
}

std::int64_t ParseIso8601ToUnixMs(std::string_view raw_value) {
  const std::string value = util::Trim(raw_value);
  if (value.size() < 19) {
    return 0;
  }

  std::tm tm = {};
  std::istringstream base_stream(value.substr(0, 19));
  base_stream >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (base_stream.fail()) {
    return 0;
  }

  std::time_t seconds = FERRYMAN_TIMEGM(&tm);
  if (seconds < 0) {
    return 0;
  }

  std::int64_t millis = static_cast<std::int64_t>(seconds) * 1000;
  size_t cursor = 19;
  if (cursor < value.size() && value[cursor] == '.') {
    ++cursor;
    size_t end = cursor;
    while (end < value.size() && std::isdigit(static_cast<unsigned char>(value[end]))) {
      ++end;
    }
    std::string fractional = value.substr(cursor, end - cursor);
    if (!fractional.empty()) {
      if (fractional.size() > 3) {
        fractional.resize(3);
      }
      while (fractional.size() < 3) {
        fractional.push_back('0');
      }
      millis += std::strtol(fractional.c_str(), nullptr, 10);
    }
    cursor = end;
  }

  if (cursor >= value.size() || value[cursor] == 'Z' || value[cursor] == 'z') {
    return millis;
  }

  if (value[cursor] != '+' && value[cursor] != '-') {
    return millis;
  }

  const char sign = value[cursor++];
  if (cursor + 4 >= value.size()) {
    return millis;
  }
  const int hours = std::strtol(value.substr(cursor, 2).c_str(), nullptr, 10);
  cursor += 2;
  if (cursor < value.size() && value[cursor] == ':') {
    ++cursor;
  }
  const int minutes = std::strtol(value.substr(cursor, 2).c_str(), nullptr, 10);
  const std::int64_t offset_ms = static_cast<std::int64_t>((hours * 60) + minutes) * 60 * 1000;
  if (sign == '+') {
    millis -= offset_ms;
  } else {
    millis += offset_ms;
  }
  return millis;
}

std::optional<nlohmann::json> ParseJsonLine(const std::string& line) {
  if (util::Trim(line).empty()) {
    return std::nullopt;
  }
  try {
    return nlohmann::json::parse(line);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string> JsonStringValue(const nlohmann::json& value, std::initializer_list<const char*> keys) {
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
}

const nlohmann::json* JsonField(const nlohmann::json& value, std::initializer_list<const char*> keys) {
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
}

std::optional<nlohmann::json> ParseMaybeJson(const std::string& value) {
  const std::string trimmed = util::Trim(value);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  if (trimmed.front() != '{' && trimmed.front() != '[') {
    return std::nullopt;
  }
  try {
    return nlohmann::json::parse(trimmed);
  } catch (...) {
    return std::nullopt;
  }
}

bool IsUtf8ContinuationByte(unsigned char value) {
  return (value & 0xC0u) == 0x80u;
}

size_t Utf8SequenceLength(unsigned char lead_byte) {
  if (lead_byte <= 0x7Fu) {
    return 1;
  }
  if (lead_byte >= 0xC2u && lead_byte <= 0xDFu) {
    return 2;
  }
  if (lead_byte >= 0xE0u && lead_byte <= 0xEFu) {
    return 3;
  }
  if (lead_byte >= 0xF0u && lead_byte <= 0xF4u) {
    return 4;
  }
  return 0;
}

bool IsValidUtf8Sequence(std::string_view value, size_t offset, size_t length) {
  if (length == 0 || offset + length > value.size()) {
    return false;
  }
  const unsigned char lead_byte = static_cast<unsigned char>(value[offset]);
  if (length == 1) {
    return lead_byte <= 0x7Fu;
  }
  for (size_t index = 1; index < length; ++index) {
    if (!IsUtf8ContinuationByte(static_cast<unsigned char>(value[offset + index]))) {
      return false;
    }
  }
  const unsigned char next_byte = static_cast<unsigned char>(value[offset + 1]);
  if (length == 2) {
    return lead_byte >= 0xC2u && lead_byte <= 0xDFu;
  }
  if (length == 3) {
    if (lead_byte == 0xE0u && next_byte < 0xA0u) {
      return false;
    }
    if (lead_byte == 0xEDu && next_byte >= 0xA0u) {
      return false;
    }
    return lead_byte >= 0xE0u && lead_byte <= 0xEFu;
  }
  if (lead_byte == 0xF0u && next_byte < 0x90u) {
    return false;
  }
  if (lead_byte == 0xF4u && next_byte > 0x8Fu) {
    return false;
  }
  return lead_byte >= 0xF0u && lead_byte <= 0xF4u;
}

std::string SanitizeUtf8(std::string_view value) {
  std::string sanitized;
  sanitized.reserve(value.size());
  static constexpr std::string_view kReplacement = "\xEF\xBF\xBD";

  for (size_t offset = 0; offset < value.size();) {
    const unsigned char lead_byte = static_cast<unsigned char>(value[offset]);
    const size_t sequence_length = Utf8SequenceLength(lead_byte);
    if (sequence_length > 0 && IsValidUtf8Sequence(value, offset, sequence_length)) {
      sanitized.append(value.substr(offset, sequence_length));
      offset += sequence_length;
      continue;
    }
    sanitized.append(kReplacement);
    ++offset;
  }
  return sanitized;
}

size_t Utf8SafePrefixLength(std::string_view value, size_t max_bytes) {
  size_t offset = 0;
  size_t last_valid = 0;
  while (offset < value.size() && offset < max_bytes) {
    const unsigned char lead_byte = static_cast<unsigned char>(value[offset]);
    const size_t sequence_length = Utf8SequenceLength(lead_byte);
    if (sequence_length == 0 || !IsValidUtf8Sequence(value, offset, sequence_length)) {
      if (offset + 1 > max_bytes) {
        break;
      }
      last_valid = offset + 1;
      offset += 1;
      continue;
    }
    if (offset + sequence_length > max_bytes) {
      break;
    }
    last_valid = offset + sequence_length;
    offset += sequence_length;
  }
  return last_valid;
}

std::string TruncateText(std::string text, size_t max_chars) {
  text = util::Trim(SanitizeUtf8(text));
  if (text.size() <= max_chars) {
    return text;
  }
  if (max_chars <= 3) {
    return text.substr(0, Utf8SafePrefixLength(text, max_chars));
  }
  return text.substr(0, Utf8SafePrefixLength(text, max_chars - 3)) + "...";
}

std::string BasenameForDisplay(const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }
  const std::string filename = path.filename().string();
  if (!filename.empty()) {
    return filename;
  }
  return path.string();
}

std::string JoinCodexAssistantText(const nlohmann::json& content) {
  if (!content.is_array()) {
    return {};
  }
  std::string combined;
  for (const auto& block : content) {
    if (!block.is_object()) {
      continue;
    }
    const std::string block_type = block.value("type", std::string());
    std::string text;
    if (block_type == "output_text") {
      text = block.value("text", std::string());
    } else if (block_type == "text") {
      text = block.value("text", std::string());
    }
    text = util::Trim(text);
    if (text.empty()) {
      continue;
    }
    if (!combined.empty()) {
      combined += "\n\n";
    }
    combined += text;
  }
  return combined;
}

std::string JoinClaudeTextBlocks(const nlohmann::json& content) {
  if (content.is_string()) {
    return util::Trim(content.get<std::string>());
  }
  if (!content.is_array()) {
    return {};
  }
  std::string combined;
  for (const auto& block : content) {
    if (!block.is_object()) {
      continue;
    }
    const std::string block_type = block.value("type", std::string());
    std::string text;
    if (block_type == "text") {
      text = block.value("text", std::string());
    } else if (block_type == "thinking") {
      text = block.value("thinking", std::string());
    }
    text = util::Trim(text);
    if (text.empty()) {
      continue;
    }
    if (!combined.empty()) {
      combined += "\n\n";
    }
    combined += text;
  }
  return combined;
}

std::optional<std::string> ExtractTitleFromToolUse(std::string name, const nlohmann::json& input) {
  name = ToLowerCopy(util::Trim(name));
  if (name.empty()) {
    return std::nullopt;
  }
  if (name.find("change_title") == std::string::npos && name.find("change-title") == std::string::npos &&
      name.find("title") == std::string::npos) {
    return std::nullopt;
  }
  if (!input.is_object()) {
    return std::nullopt;
  }
  auto title = JsonStringValue(input, {"title", "name", "text"});
  if (!title.has_value()) {
    return std::nullopt;
  }
  return TruncateText(*title, 120);
}

nlohmann::json WrapUserTextMessage(const std::string& text) {
  return {
      {"role", "user"},
      {"content", {{"type", "text"}, {"text", text}}},
      {"meta", {{"sentFrom", "cli"}}},
  };
}

nlohmann::json WrapCodexBodyMessage(nlohmann::json body) {
  return {
      {"role", "agent"},
      {"content", {{"type", "codex"}, {"data", std::move(body)}}},
      {"meta", {{"sentFrom", "cli"}}},
  };
}

bool IsJsonlFile(const std::filesystem::path& path) {
  return path.has_extension() && ToLowerCopy(path.extension().string()) == ".jsonl";
}

struct SqliteDbCloser {
  void operator()(sqlite3* db) const {
    if (db != nullptr) {
      sqlite3_close(db);
    }
  }
};

struct SqliteStmtCloser {
  void operator()(sqlite3_stmt* stmt) const {
    if (stmt != nullptr) {
      sqlite3_finalize(stmt);
    }
  }
};

using SqliteDbPtr = std::unique_ptr<sqlite3, SqliteDbCloser>;
using SqliteStmtPtr = std::unique_ptr<sqlite3_stmt, SqliteStmtCloser>;

SqliteDbPtr OpenReadOnlySqlite(const std::filesystem::path& db_path) {
  if (db_path.empty()) {
    return nullptr;
  }

  sqlite3* raw_db = nullptr;
  if (sqlite3_open_v2(db_path.string().c_str(), &raw_db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (raw_db != nullptr) {
      sqlite3_close(raw_db);
    }
    return nullptr;
  }
  return SqliteDbPtr(raw_db);
}

std::optional<std::string> ReadSqliteItemValue(sqlite3* db, std::string_view key,
                                               std::string_view table_name = "ItemTable") {
  if (db == nullptr || key.empty() || table_name.empty()) {
    return std::nullopt;
  }

  const std::string sql = "SELECT value FROM " + std::string(table_name) + " WHERE key = ? LIMIT 1";
  sqlite3_stmt* raw_stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw_stmt, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  SqliteStmtPtr stmt(raw_stmt);

  const std::string key_text(key);
  if (sqlite3_bind_text(stmt.get(), 1, key_text.c_str(), static_cast<int>(key_text.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
    return std::nullopt;
  }

  if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
    return std::nullopt;
  }

  const unsigned char* value = sqlite3_column_text(stmt.get(), 0);
  if (value == nullptr) {
    return std::string();
  }
  return std::string(reinterpret_cast<const char*>(value));
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    return std::nullopt;
  }
  std::ostringstream stream;
  stream << input.rdbuf();
  return stream.str();
}

std::int64_t NormalizeUnixTimestampToMs(std::int64_t value) {
  if (value <= 0) {
    return 0;
  }
  if (value < 100000000000LL) {
    return value * 1000;
  }
  return value;
}

std::int64_t JsonValueToUnixMs(const nlohmann::json& value) {
  if (value.is_number_integer() || value.is_number_unsigned()) {
    return NormalizeUnixTimestampToMs(value.get<std::int64_t>());
  }
  if (value.is_number_float()) {
    return NormalizeUnixTimestampToMs(static_cast<std::int64_t>(value.get<double>()));
  }
  if (!value.is_string()) {
    return 0;
  }

  const std::string text = util::Trim(value.get<std::string>());
  if (text.empty()) {
    return 0;
  }
  const bool all_digits = std::all_of(text.begin(), text.end(), [](unsigned char c) {
    return std::isdigit(c) != 0;
  });
  if (all_digits) {
    return NormalizeUnixTimestampToMs(std::strtoll(text.c_str(), nullptr, 10));
  }
  return ParseIso8601ToUnixMs(text);
}

std::int64_t JsonTimestampValue(const nlohmann::json& value, std::initializer_list<const char*> keys) {
  if (!value.is_object()) {
    return 0;
  }
  for (const char* key : keys) {
    auto it = value.find(key);
    if (it == value.end()) {
      continue;
    }
    const std::int64_t parsed = JsonValueToUnixMs(*it);
    if (parsed > 0) {
      return parsed;
    }
  }
  return 0;
}

void AppendJoinedText(std::string* combined, std::string text, std::string_view separator = "\n\n") {
  if (combined == nullptr) {
    return;
  }
  text = util::Trim(std::move(text));
  if (text.empty()) {
    return;
  }
  if (!combined->empty()) {
    combined->append(separator);
  }
  combined->append(text);
}

std::string JoinJsonText(const nlohmann::json& value, std::string_view separator = "\n\n") {
  if (value.is_null()) {
    return {};
  }
  if (value.is_string()) {
    return util::Trim(value.get<std::string>());
  }
  if (value.is_array()) {
    std::string combined;
    for (const auto& item : value) {
      AppendJoinedText(&combined, JoinJsonText(item, separator), separator);
    }
    return combined;
  }
  if (!value.is_object()) {
    return {};
  }

  if (auto text = JsonStringValue(value, {"text"}); text.has_value()) {
    return *text;
  }
  if (const auto* content = JsonField(value, {"content"}); content != nullptr) {
    const std::string nested = JoinJsonText(*content, separator);
    if (!nested.empty()) {
      return nested;
    }
  }
  if (const auto* message = JsonField(value, {"message"}); message != nullptr) {
    const std::string nested = JoinJsonText(*message, separator);
    if (!nested.empty()) {
      return nested;
    }
  }
  if (const auto* parts = JsonField(value, {"parts"}); parts != nullptr) {
    const std::string nested = JoinJsonText(*parts, separator);
    if (!nested.empty()) {
      return nested;
    }
  }
  if (const auto* body = JsonField(value, {"body"}); body != nullptr) {
    const std::string nested = JoinJsonText(*body, separator);
    if (!nested.empty()) {
      return nested;
    }
  }
  if (const auto* value_field = JsonField(value, {"value"}); value_field != nullptr) {
    const std::string nested = JoinJsonText(*value_field, separator);
    if (!nested.empty()) {
      return nested;
    }
  }
  if (const auto* response = JsonField(value, {"response"}); response != nullptr) {
    const std::string nested = JoinJsonText(*response, separator);
    if (!nested.empty()) {
      return nested;
    }
  }
  return {};
}

std::string ExtractCursorUserText(const nlohmann::json& request) {
  if (!request.is_object()) {
    return {};
  }
  if (const auto* message = JsonField(request, {"message"}); message != nullptr) {
    return JoinJsonText(*message);
  }
  return {};
}

std::string ExtractCursorAssistantText(const nlohmann::json& request) {
  if (!request.is_object()) {
    return {};
  }

  if (const auto* result = JsonField(request, {"result"}); result != nullptr && result->is_object()) {
    if (const auto* metadata = JsonField(*result, {"metadata"}); metadata != nullptr && metadata->is_object()) {
      if (const auto* messages = JsonField(*metadata, {"messages"}); messages != nullptr && messages->is_array()) {
        std::string combined;
        for (const auto& entry : *messages) {
          if (!entry.is_object()) {
            continue;
          }
          const std::string role = ToLowerCopy(entry.value("role", std::string()));
          if (role != "assistant" && role != "agent" && role != "model") {
            continue;
          }
          AppendJoinedText(&combined, JoinJsonText(entry.value("content", nlohmann::json(nullptr))));
        }
        if (!combined.empty()) {
          return combined;
        }
      }
    }
  }

  if (const auto* response = JsonField(request, {"response"}); response != nullptr) {
    return JoinJsonText(*response);
  }
  return {};
}

int HexDigitValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (c - 'A');
  }
  return -1;
}

std::string PercentDecode(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '%' && index + 2 < value.size()) {
      const int hi = HexDigitValue(value[index + 1]);
      const int lo = HexDigitValue(value[index + 2]);
      if (hi >= 0 && lo >= 0) {
        result.push_back(static_cast<char>((hi << 4) | lo));
        index += 2;
        continue;
      }
    }
    result.push_back(value[index]);
  }
  return result;
}

std::filesystem::path FileUriToPath(std::string_view raw_uri) {
  const std::string uri = util::Trim(raw_uri);
  if (!uri.starts_with("file://")) {
    return std::filesystem::path(uri);
  }

  std::string path_text = uri.substr(std::string_view("file://").size());
  if (path_text.starts_with("localhost/")) {
    path_text = path_text.substr(std::string("localhost").size());
  }
  path_text = PercentDecode(path_text);
#if defined(_WIN32)
  if (path_text.size() >= 3 && path_text[0] == '/' && std::isalpha(static_cast<unsigned char>(path_text[1])) && path_text[2] == ':') {
    path_text.erase(path_text.begin());
  }
#endif
  return std::filesystem::path(path_text);
}

std::filesystem::path ReadCursorWorkspacePath(const std::filesystem::path& workspace_storage_dir) {
  const auto workspace_json = ReadTextFile(workspace_storage_dir / "workspace.json");
  if (!workspace_json.has_value()) {
    return {};
  }
  const auto parsed = ParseMaybeJson(*workspace_json);
  if (!parsed.has_value() || !parsed->is_object()) {
    return {};
  }
  if (auto folder = JsonStringValue(*parsed, {"folder", "workspace", "path"}); folder.has_value()) {
    return FileUriToPath(*folder);
  }
  return {};
}

LocalSessionSummary ReadCursorSessionHeader(const nlohmann::json& session_json, const std::filesystem::path& transcript_path,
                                            const std::filesystem::path& workspace_path,
                                            std::int64_t fallback_updated_at_ms) {
  LocalSessionSummary summary;
  summary.transcript_path = transcript_path;
  summary.cwd = workspace_path;
  summary.permission_mode = policy::DefaultPermissionModeForFlavor("cursor");
  summary.updated_at_ms = fallback_updated_at_ms;
  summary.created_at_ms = fallback_updated_at_ms;

  if (!session_json.is_object()) {
    return summary;
  }

  if (auto session_id = JsonStringValue(session_json, {"sessionId", "id"}); session_id.has_value()) {
    summary.source_session_id = *session_id;
  }
  if (auto name = JsonStringValue(session_json, {"customTitle", "title", "name"}); name.has_value()) {
    summary.name = *name;
  }
  const std::int64_t created_at_ms = JsonTimestampValue(session_json, {"creationDate", "createdAt", "startTime"});
  const std::int64_t updated_at_ms = JsonTimestampValue(session_json, {"lastMessageDate", "lastUpdated", "updatedAt"});
  if (created_at_ms > 0) {
    summary.created_at_ms = created_at_ms;
  }
  if (updated_at_ms > 0) {
    summary.updated_at_ms = updated_at_ms;
  }

  std::string first_user_text;
  std::string last_assistant_text;
  if (const auto* requests = JsonField(session_json, {"requests"}); requests != nullptr && requests->is_array()) {
    for (const auto& request : *requests) {
      const std::int64_t request_ts = JsonTimestampValue(request, {"timestamp", "createdAt", "updatedAt"});
      if (request_ts > 0) {
        if (summary.created_at_ms <= 0 || request_ts < summary.created_at_ms) {
          summary.created_at_ms = request_ts;
        }
        summary.updated_at_ms = std::max(summary.updated_at_ms, request_ts);
      }
      const std::string user_text = ExtractCursorUserText(request);
      if (first_user_text.empty() && !user_text.empty()) {
        first_user_text = user_text;
      }
      const std::string assistant_text = ExtractCursorAssistantText(request);
      if (!assistant_text.empty()) {
        last_assistant_text = assistant_text;
      }
    }
  }

  if (summary.name.empty()) {
    if (!first_user_text.empty()) {
      summary.name = TruncateText(first_user_text, 80);
    } else if (!summary.cwd.empty()) {
      summary.name = BasenameForDisplay(summary.cwd);
    } else {
      summary.name = summary.source_session_id;
    }
  }

  if (!last_assistant_text.empty()) {
    summary.summary_text = TruncateText(last_assistant_text, 120);
  } else if (!first_user_text.empty()) {
    summary.summary_text = TruncateText(first_user_text, 120);
  } else {
    summary.summary_text = summary.name;
  }
  return summary;
}

std::unordered_map<std::string, std::filesystem::path> ReadGeminiProjectMap(const std::filesystem::path& home_path) {
  std::unordered_map<std::string, std::filesystem::path> result;
  const auto project_map_text = ReadTextFile(home_path / ".gemini" / "tmp" / "project-map.json");
  if (!project_map_text.has_value()) {
    return result;
  }
  const auto parsed = ParseMaybeJson(*project_map_text);
  if (!parsed.has_value() || !parsed->is_object()) {
    return result;
  }

  for (auto it = parsed->begin(); it != parsed->end(); ++it) {
    if (!it.value().is_string()) {
      continue;
    }
    result[it.key()] = std::filesystem::path(it.value().get<std::string>());
  }
  return result;
}

std::string ExtractGeminiContentText(const nlohmann::json& content) {
  return JoinJsonText(content, "\n\n");
}

LocalSessionSummary ReadGeminiSessionHeader(const std::filesystem::path& transcript_path,
                                            const std::unordered_map<std::string, std::filesystem::path>& project_map) {
  LocalSessionSummary summary;
  summary.transcript_path = transcript_path;
  summary.permission_mode = policy::DefaultPermissionModeForFlavor("gemini");

  std::error_code ec;
  summary.updated_at_ms = FileTimeToUnixMs(std::filesystem::last_write_time(transcript_path, ec));
  summary.created_at_ms = summary.updated_at_ms;

  const auto raw_text = ReadTextFile(transcript_path);
  if (!raw_text.has_value()) {
    return summary;
  }
  const auto parsed = ParseMaybeJson(*raw_text);
  if (!parsed.has_value() || !parsed->is_object()) {
    return summary;
  }
  const nlohmann::json& root = *parsed;

  if (auto session_id = JsonStringValue(root, {"sessionId", "id"}); session_id.has_value()) {
    summary.source_session_id = *session_id;
  }
  if (auto generated_summary = JsonStringValue(root, {"summary", "title", "name"}); generated_summary.has_value()) {
    summary.name = TruncateText(*generated_summary, 80);
    summary.summary_text = TruncateText(*generated_summary, 120);
  }
  const std::int64_t created_at_ms = JsonTimestampValue(root, {"startTime", "createdAt", "created_at"});
  const std::int64_t updated_at_ms = JsonTimestampValue(root, {"lastUpdated", "updatedAt", "updated_at"});
  if (created_at_ms > 0) {
    summary.created_at_ms = created_at_ms;
  }
  if (updated_at_ms > 0) {
    summary.updated_at_ms = updated_at_ms;
  }

  std::string project_hash;
  if (auto hash = JsonStringValue(root, {"projectHash", "project_hash"}); hash.has_value()) {
    project_hash = *hash;
  } else {
    project_hash = transcript_path.parent_path().parent_path().filename().string();
  }
  if (auto project_it = project_map.find(project_hash); project_it != project_map.end()) {
    summary.cwd = project_it->second;
  }

  std::string first_user_text;
  std::string last_assistant_text;
  if (const auto* messages = JsonField(root, {"messages"}); messages != nullptr && messages->is_array()) {
    for (const auto& message : *messages) {
      if (!message.is_object()) {
        continue;
      }
      const std::string type = ToLowerCopy(message.value("type", std::string()));
      const std::string text = ExtractGeminiContentText(message.value("content", nlohmann::json(nullptr)));
      if (type == "user") {
        if (first_user_text.empty() && !text.empty()) {
          first_user_text = text;
        }
        continue;
      }
      if (type == "gemini") {
        if (summary.model.empty()) {
          if (auto model = JsonStringValue(message, {"model"}); model.has_value()) {
            summary.model = *model;
          }
        }
        if (!text.empty()) {
          last_assistant_text = text;
        }
      }
    }
  }

  if (summary.name.empty()) {
    if (!first_user_text.empty()) {
      summary.name = TruncateText(first_user_text, 80);
    } else if (!summary.cwd.empty()) {
      summary.name = BasenameForDisplay(summary.cwd);
    } else {
      summary.name = summary.source_session_id;
    }
  }
  if (summary.summary_text.empty()) {
    if (!last_assistant_text.empty()) {
      summary.summary_text = TruncateText(last_assistant_text, 120);
    } else if (!first_user_text.empty()) {
      summary.summary_text = TruncateText(first_user_text, 120);
    } else {
      summary.summary_text = summary.name;
    }
  }
  return summary;
}

bool LooksLikeMessageObject(const nlohmann::json& value) {
  if (!value.is_object()) {
    return false;
  }
  if (value.contains("role") || value.contains("sender")) {
    return true;
  }
  if (value.contains("type") &&
      (value.contains("content") || value.contains("message") || value.contains("text") || value.contains("toolCalls"))) {
    return true;
  }
  if (value.contains("functionCall") || value.contains("functionResponse") || value.contains("toolCall") ||
      value.contains("toolResult")) {
    return true;
  }
  return false;
}

const nlohmann::json* FindLikelyMessageArray(const nlohmann::json& value, int depth = 0) {
  if (depth > 8) {
    return nullptr;
  }
  if (value.is_array()) {
    size_t score = 0;
    for (const auto& item : value) {
      if (LooksLikeMessageObject(item) || item.is_string()) {
        ++score;
      }
      if (score >= 1) {
        return &value;
      }
    }
    for (const auto& item : value) {
      if (const auto* nested = FindLikelyMessageArray(item, depth + 1); nested != nullptr) {
        return nested;
      }
    }
    return nullptr;
  }
  if (!value.is_object()) {
    return nullptr;
  }

  for (const char* key : {"messages", "conversation", "entries", "events", "items", "transcript", "history"}) {
    auto it = value.find(key);
    if (it != value.end()) {
      if (const auto* nested = FindLikelyMessageArray(*it, depth + 1); nested != nullptr) {
        return nested;
      }
    }
  }
  for (auto it = value.begin(); it != value.end(); ++it) {
    if (const auto* nested = FindLikelyMessageArray(it.value(), depth + 1); nested != nullptr) {
      return nested;
    }
  }
  return nullptr;
}

std::string ExtractOpencodeMessageText(const nlohmann::json& message) {
  if (!message.is_object()) {
    return JoinJsonText(message);
  }
  if (const auto* content = JsonField(message, {"content", "message", "text", "body", "value"}); content != nullptr) {
    return JoinJsonText(*content);
  }
  return {};
}

LocalSessionSummary ReadOpencodeSessionHeader(const nlohmann::json& root, const std::filesystem::path& transcript_path,
                                              std::int64_t fallback_updated_at_ms) {
  LocalSessionSummary summary;
  summary.transcript_path = transcript_path;
  summary.permission_mode = policy::DefaultPermissionModeForFlavor("opencode");
  summary.updated_at_ms = fallback_updated_at_ms;
  summary.created_at_ms = fallback_updated_at_ms;

  if (!root.is_object()) {
    return summary;
  }

  if (auto session_id = JsonStringValue(root, {"sessionId", "session_id", "id", "uuid"}); session_id.has_value()) {
    summary.source_session_id = *session_id;
  }
  if (auto name = JsonStringValue(root, {"title", "name", "summary"}); name.has_value()) {
    summary.name = TruncateText(*name, 80);
    summary.summary_text = TruncateText(*name, 120);
  }
  const std::int64_t created_at_ms = JsonTimestampValue(root, {"startTime", "createdAt", "created_at", "timestamp"});
  const std::int64_t updated_at_ms = JsonTimestampValue(root, {"lastUpdated", "updatedAt", "updated_at", "timestamp"});
  if (created_at_ms > 0) {
    summary.created_at_ms = created_at_ms;
  }
  if (updated_at_ms > 0) {
    summary.updated_at_ms = updated_at_ms;
  }

  if (auto cwd = JsonStringValue(root, {"cwd", "path", "projectPath"}); cwd.has_value()) {
    summary.cwd = FileUriToPath(*cwd);
  } else if (const auto* project = JsonField(root, {"project"}); project != nullptr && project->is_object()) {
    if (auto cwd = JsonStringValue(*project, {"path", "cwd", "root"}); cwd.has_value()) {
      summary.cwd = FileUriToPath(*cwd);
    }
  }

  std::string first_user_text;
  std::string last_assistant_text;
  if (const auto* messages = FindLikelyMessageArray(root); messages != nullptr && messages->is_array()) {
    for (const auto& message : *messages) {
      if (!message.is_object()) {
        continue;
      }
      const std::string role = ToLowerCopy(message.value("role", message.value("sender", message.value("type", std::string()))));
      const std::string text = ExtractOpencodeMessageText(message);
      if ((role == "user" || role == "human") && first_user_text.empty() && !text.empty()) {
        first_user_text = text;
      }
      if ((role == "assistant" || role == "agent" || role == "model" || role == "opencode") && !text.empty()) {
        last_assistant_text = text;
      }
    }
  }

  if (summary.name.empty()) {
    if (!first_user_text.empty()) {
      summary.name = TruncateText(first_user_text, 80);
    } else if (!summary.cwd.empty()) {
      summary.name = BasenameForDisplay(summary.cwd);
    } else {
      summary.name = summary.source_session_id;
    }
  }
  if (summary.summary_text.empty()) {
    if (!last_assistant_text.empty()) {
      summary.summary_text = TruncateText(last_assistant_text, 120);
    } else if (!first_user_text.empty()) {
      summary.summary_text = TruncateText(first_user_text, 120);
    } else {
      summary.summary_text = summary.name;
    }
  }
  return summary;
}

std::unordered_map<std::string, std::filesystem::path> BuildCodexTranscriptIndex(const std::filesystem::path& home_path) {
  std::unordered_map<std::string, std::filesystem::path> result;
  const std::vector<std::filesystem::path> roots = {
      home_path / ".codex" / "sessions",
      home_path / ".codex" / "archived_sessions",
  };

  for (const auto& root : roots) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || ec) {
      continue;
    }
    if (std::filesystem::is_regular_file(root, ec)) {
      continue;
    }
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
      if (ec || !it->is_regular_file()) {
        continue;
      }
      const std::filesystem::path file_path = it->path();
      if (!IsJsonlFile(file_path)) {
        continue;
      }
      const std::string stem = file_path.stem().string();
      if (stem.size() < 36) {
        continue;
      }
      const std::string source_session_id = stem.substr(stem.size() - 36);
      if (!LooksLikeExternalSourceId(source_session_id)) {
        continue;
      }
      result[source_session_id] = file_path;
    }
  }
  return result;
}

LocalSessionSummary ReadCodexSessionHeader(const std::filesystem::path& transcript_path) {
  LocalSessionSummary summary;
  summary.transcript_path = transcript_path;
  std::error_code ec;
  summary.updated_at_ms = FileTimeToUnixMs(std::filesystem::last_write_time(transcript_path, ec));
  summary.created_at_ms = summary.updated_at_ms;
  summary.permission_mode = policy::DefaultPermissionModeForFlavor("codex");
  summary.reasoning_effort = "medium";

  std::ifstream input(transcript_path);
  if (!input.is_open()) {
    return summary;
  }

  std::string line;
  int lines_read = 0;
  while (lines_read < 12 && std::getline(input, line)) {
    ++lines_read;
    auto parsed = ParseJsonLine(line);
    if (!parsed.has_value() || !parsed->is_object()) {
      continue;
    }
    const std::string record_type = parsed->value("type", std::string());
    if (record_type == "session_meta") {
      const nlohmann::json payload = parsed->value("payload", nlohmann::json::object());
      if (auto id = JsonStringValue(payload, {"id"}); id.has_value()) {
        summary.source_session_id = *id;
      }
      if (auto cwd = JsonStringValue(payload, {"cwd"}); cwd.has_value()) {
        summary.cwd = std::filesystem::path(*cwd);
      }
      if (auto timestamp = JsonStringValue(*parsed, {"timestamp"}); timestamp.has_value()) {
        const std::int64_t created_at_ms = ParseIso8601ToUnixMs(*timestamp);
        if (created_at_ms > 0) {
          summary.created_at_ms = created_at_ms;
        }
      }
    } else if (record_type == "turn_context") {
      const nlohmann::json payload = parsed->value("payload", nlohmann::json::object());
      if (auto model = JsonStringValue(payload, {"model"}); model.has_value()) {
        summary.model = *model;
      }
      if (auto effort = JsonStringValue(payload, {"effort", "reasoning_effort", "reasoningEffort"}); effort.has_value()) {
        const std::string normalized = policy::NormalizeReasoningEffortValue(*effort);
        if (policy::IsReasoningEffortAllowedForFlavor(normalized, "codex")) {
          summary.reasoning_effort = normalized.empty() ? std::string("medium") : normalized;
        }
      }
    }
  }

  if (summary.summary_text.empty()) {
    summary.summary_text = summary.name;
  }
  return summary;
}

LocalSessionSummary ReadClaudeSessionHeader(const std::filesystem::path& transcript_path) {
  LocalSessionSummary summary;
  summary.source_session_id = transcript_path.stem().string();
  summary.transcript_path = transcript_path;
  std::error_code ec;
  summary.updated_at_ms = FileTimeToUnixMs(std::filesystem::last_write_time(transcript_path, ec));
  summary.created_at_ms = summary.updated_at_ms;
  summary.permission_mode = policy::DefaultPermissionModeForFlavor("claude");

  std::ifstream input(transcript_path);
  if (!input.is_open()) {
    return summary;
  }

  std::string first_user_text;
  std::string line;
  int lines_read = 0;
  while (lines_read < 80 && std::getline(input, line)) {
    ++lines_read;
    auto parsed = ParseJsonLine(line);
    if (!parsed.has_value() || !parsed->is_object()) {
      continue;
    }

    if (summary.created_at_ms == summary.updated_at_ms) {
      if (auto timestamp = JsonStringValue(*parsed, {"timestamp"}); timestamp.has_value()) {
        const std::int64_t created_at_ms = ParseIso8601ToUnixMs(*timestamp);
        if (created_at_ms > 0) {
          summary.created_at_ms = created_at_ms;
        }
      }
    }

    const std::string record_type = parsed->value("type", std::string());
    if (record_type == "user") {
      if (summary.cwd.empty()) {
        if (auto cwd = JsonStringValue(*parsed, {"cwd"}); cwd.has_value()) {
          summary.cwd = std::filesystem::path(*cwd);
        }
      }
      if (auto permission_mode = JsonStringValue(*parsed, {"permissionMode"}); permission_mode.has_value()) {
        summary.permission_mode = *permission_mode;
      }
      auto message_it = parsed->find("message");
      if (message_it != parsed->end() && message_it->is_object()) {
        const std::string user_text = JoinClaudeTextBlocks(message_it->value("content", nlohmann::json(nullptr)));
        if (first_user_text.empty() && !user_text.empty()) {
          first_user_text = user_text;
        }
        if (summary.summary_text.empty() && !user_text.empty()) {
          summary.summary_text = TruncateText(user_text, 120);
        }
      }
      continue;
    }

    if (record_type != "assistant") {
      continue;
    }

    auto message_it = parsed->find("message");
    if (message_it == parsed->end() || !message_it->is_object()) {
      continue;
    }
    const nlohmann::json& message = *message_it;
    if (summary.model.empty()) {
      if (auto model = JsonStringValue(message, {"model"}); model.has_value()) {
        summary.model = *model;
      }
    }

    const auto content_it = message.find("content");
    if (content_it == message.end()) {
      continue;
    }

    if (summary.name.empty() && content_it->is_array()) {
      for (const auto& block : *content_it) {
        if (!block.is_object() || block.value("type", std::string()) != "tool_use") {
          continue;
        }
        const auto title = ExtractTitleFromToolUse(block.value("name", std::string()), block.value("input", nlohmann::json::object()));
        if (title.has_value()) {
          summary.name = *title;
          break;
        }
      }
    }

    if (summary.summary_text.empty()) {
      const std::string assistant_text = JoinClaudeTextBlocks(*content_it);
      if (!assistant_text.empty()) {
        summary.summary_text = TruncateText(assistant_text, 120);
      }
    }
  }

  if (summary.name.empty()) {
    if (!first_user_text.empty()) {
      summary.name = TruncateText(first_user_text, 80);
    } else {
      summary.name = BasenameForDisplay(summary.cwd);
    }
  }
  if (summary.summary_text.empty()) {
    summary.summary_text = summary.name;
  }
  return summary;
}

}  // namespace

std::string CodeAgentManager::MakeExternalSessionId(std::string_view flavor, std::string_view source_session_id) {
  return "external-" + std::string(flavor) + "-" + std::string(source_session_id);
}

bool CodeAgentManager::ParseExternalSessionId(std::string_view session_id, std::string* flavor,
                                              std::string* source_session_id) {
  static constexpr std::string_view kPrefix = "external-";
  if (!session_id.starts_with(kPrefix)) {
    return false;
  }
  const std::string_view remainder = session_id.substr(kPrefix.size());
  const size_t separator = remainder.find('-');
  if (separator == std::string_view::npos || separator == 0 || separator + 1 >= remainder.size()) {
    return false;
  }
  if (flavor != nullptr) {
    *flavor = std::string(remainder.substr(0, separator));
  }
  if (source_session_id != nullptr) {
    *source_session_id = std::string(remainder.substr(separator + 1));
  }
  return true;
}

std::vector<CodeAgentManager::SessionRecord> CodeAgentManager::DiscoverExternalSessions(const std::string& ns) const {
  const std::string normalized_ns = util::Trim(ns);
  if (!normalized_ns.empty() && normalized_ns != "default") {
    return {};
  }

  const std::string home = GetHomeDir();
  if (home.empty()) {
    return {};
  }
  const std::filesystem::path home_path(home);

  std::vector<SessionRecord> sessions;

  const auto codex_transcripts = BuildCodexTranscriptIndex(home_path);
  const std::filesystem::path codex_index_path = home_path / ".codex" / "session_index.jsonl";
  {
    std::ifstream input(codex_index_path);
    std::string line;
    while (std::getline(input, line)) {
      auto parsed = ParseJsonLine(line);
      if (!parsed.has_value() || !parsed->is_object()) {
        continue;
      }
      const auto source_session_id = JsonStringValue(*parsed, {"id"});
      if (!source_session_id.has_value()) {
        continue;
      }
      auto transcript_it = codex_transcripts.find(*source_session_id);
      if (transcript_it == codex_transcripts.end()) {
        continue;
      }

      LocalSessionSummary summary = ReadCodexSessionHeader(transcript_it->second);
      summary.source_session_id = *source_session_id;
      if (auto title = JsonStringValue(*parsed, {"thread_name", "title", "name"}); title.has_value()) {
        summary.name = *title;
      }
      if (auto updated_at = JsonStringValue(*parsed, {"updated_at", "updatedAt", "timestamp"}); updated_at.has_value()) {
        const std::int64_t parsed_updated_at = ParseIso8601ToUnixMs(*updated_at);
        if (parsed_updated_at > 0) {
          summary.updated_at_ms = parsed_updated_at;
        }
      }
      if (summary.name.empty()) {
        summary.name = BasenameForDisplay(summary.cwd);
      }
      if (summary.summary_text.empty()) {
        summary.summary_text = summary.name;
      }

      SessionRecord session;
      session.id = MakeExternalSessionId("codex", summary.source_session_id);
      session.ns = "default";
      session.active = false;
      session.thinking = false;
      session.created_at_ms = summary.created_at_ms > 0 ? summary.created_at_ms : summary.updated_at_ms;
      session.updated_at_ms = summary.updated_at_ms;
      session.active_at_ms = session.updated_at_ms;
      session.thinking_at_ms = session.updated_at_ms;
      session.path = summary.cwd;
      session.transcript_path = summary.transcript_path;
      session.host = "localhost";
      session.name = summary.name;
      session.summary_text = summary.summary_text;
      session.summary_updated_at_ms = session.updated_at_ms;
      session.flavor = "codex";
      session.source_session_id = summary.source_session_id;
      session.permission_mode = policy::CanonicalizePermissionModeForFlavor(summary.permission_mode, session.flavor);
      if (session.permission_mode.empty()) {
        session.permission_mode = policy::DefaultPermissionModeForFlavor(session.flavor);
      }
      session.model_reasoning_effort = policy::NormalizeReasoningEffortValue(summary.reasoning_effort);
      if (!policy::IsReasoningEffortAllowedForFlavor(session.model_reasoning_effort, session.flavor)) {
        session.model_reasoning_effort = "medium";
      }
      session.codex_fast = summary.codex_fast;
      session.model = summary.model;
      session.title_initialized = !session.name.empty();
      sessions.push_back(std::move(session));
    }
  }

  const std::filesystem::path claude_projects_path = home_path / ".claude" / "projects";
  {
    std::error_code ec;
    if (std::filesystem::exists(claude_projects_path, ec) && !ec) {
      for (std::filesystem::recursive_directory_iterator it(claude_projects_path, ec), end; !ec && it != end; it.increment(ec)) {
        if (ec || !it->is_regular_file()) {
          continue;
        }
        const std::filesystem::path transcript_path = it->path();
        if (!IsJsonlFile(transcript_path)) {
          continue;
        }

        LocalSessionSummary summary = ReadClaudeSessionHeader(transcript_path);
        if (summary.source_session_id.empty()) {
          continue;
        }
        SessionRecord session;
        session.id = MakeExternalSessionId("claude", summary.source_session_id);
        session.ns = "default";
        session.active = false;
        session.thinking = false;
        session.created_at_ms = summary.created_at_ms > 0 ? summary.created_at_ms : summary.updated_at_ms;
        session.updated_at_ms = summary.updated_at_ms;
        session.active_at_ms = session.updated_at_ms;
        session.thinking_at_ms = session.updated_at_ms;
        session.path = summary.cwd;
        session.transcript_path = summary.transcript_path;
        session.host = "localhost";
        session.name = summary.name.empty() ? BasenameForDisplay(summary.cwd) : summary.name;
        session.summary_text = summary.summary_text;
        session.summary_updated_at_ms = session.updated_at_ms;
        session.flavor = "claude";
        session.source_session_id = summary.source_session_id;
        session.permission_mode = policy::CanonicalizePermissionModeForFlavor(summary.permission_mode, session.flavor);
        if (session.permission_mode.empty()) {
          session.permission_mode = policy::DefaultPermissionModeForFlavor(session.flavor);
        }
        session.model = summary.model;
        session.title_initialized = !session.name.empty();
        sessions.push_back(std::move(session));
      }
    }
  }

  const std::filesystem::path cursor_storage_path =
      home_path / "Library" / "Application Support" / "Cursor" / "User" / "workspaceStorage";
  {
    std::error_code ec;
    if (std::filesystem::exists(cursor_storage_path, ec) && !ec) {
      for (std::filesystem::directory_iterator it(cursor_storage_path, ec), end; !ec && it != end; it.increment(ec)) {
        if (ec || !it->is_directory()) {
          continue;
        }
        const std::filesystem::path workspace_storage_dir = it->path();
        const std::filesystem::path state_db_path = workspace_storage_dir / "state.vscdb";
        if (!std::filesystem::exists(state_db_path, ec) || ec) {
          continue;
        }

        auto db = OpenReadOnlySqlite(state_db_path);
        if (!db) {
          continue;
        }
        const auto raw_sessions = ReadSqliteItemValue(db.get(), "interactive.sessions");
        if (!raw_sessions.has_value()) {
          continue;
        }
        const auto parsed = ParseMaybeJson(*raw_sessions);
        if (!parsed.has_value() || !parsed->is_array()) {
          continue;
        }

        const std::filesystem::path workspace_path = ReadCursorWorkspacePath(workspace_storage_dir);
        const std::int64_t db_updated_at_ms = FileTimeToUnixMs(std::filesystem::last_write_time(state_db_path, ec));
        for (const auto& session_json : *parsed) {
          LocalSessionSummary summary = ReadCursorSessionHeader(session_json, state_db_path, workspace_path, db_updated_at_ms);
          if (summary.source_session_id.empty()) {
            continue;
          }

          SessionRecord session;
          session.id = MakeExternalSessionId("cursor", summary.source_session_id);
          session.ns = "default";
          session.active = false;
          session.thinking = false;
          session.created_at_ms = summary.created_at_ms > 0 ? summary.created_at_ms : summary.updated_at_ms;
          session.updated_at_ms = summary.updated_at_ms;
          session.active_at_ms = session.updated_at_ms;
          session.thinking_at_ms = session.updated_at_ms;
          session.path = summary.cwd;
          session.transcript_path = summary.transcript_path;
          session.host = "localhost";
          session.name = summary.name;
          session.summary_text = summary.summary_text;
          session.summary_updated_at_ms = session.updated_at_ms;
          session.flavor = "cursor";
          session.source_session_id = summary.source_session_id;
          session.permission_mode = policy::CanonicalizePermissionModeForFlavor(summary.permission_mode, session.flavor);
          if (session.permission_mode.empty()) {
            session.permission_mode = policy::DefaultPermissionModeForFlavor(session.flavor);
          }
          session.model = summary.model;
          session.title_initialized = !session.name.empty();
          sessions.push_back(std::move(session));
        }
      }
    }
  }

  const auto gemini_project_map = ReadGeminiProjectMap(home_path);
  const std::filesystem::path gemini_tmp_path = home_path / ".gemini" / "tmp";
  {
    std::error_code ec;
    if (std::filesystem::exists(gemini_tmp_path, ec) && !ec) {
      for (std::filesystem::recursive_directory_iterator it(gemini_tmp_path, ec), end; !ec && it != end; it.increment(ec)) {
        if (ec || !it->is_regular_file()) {
          continue;
        }
        const std::filesystem::path transcript_path = it->path();
        if (ToLowerCopy(transcript_path.extension().string()) != ".json") {
          continue;
        }
        const std::string filename = transcript_path.filename().string();
        if (!filename.starts_with("session-") || transcript_path.parent_path().filename() != "chats") {
          continue;
        }

        LocalSessionSummary summary = ReadGeminiSessionHeader(transcript_path, gemini_project_map);
        if (summary.source_session_id.empty()) {
          continue;
        }

        SessionRecord session;
        session.id = MakeExternalSessionId("gemini", summary.source_session_id);
        session.ns = "default";
        session.active = false;
        session.thinking = false;
        session.created_at_ms = summary.created_at_ms > 0 ? summary.created_at_ms : summary.updated_at_ms;
        session.updated_at_ms = summary.updated_at_ms;
        session.active_at_ms = session.updated_at_ms;
        session.thinking_at_ms = session.updated_at_ms;
        session.path = summary.cwd;
        session.transcript_path = summary.transcript_path;
        session.host = "localhost";
        session.name = summary.name;
        session.summary_text = summary.summary_text;
        session.summary_updated_at_ms = session.updated_at_ms;
        session.flavor = "gemini";
        session.source_session_id = summary.source_session_id;
        session.permission_mode = policy::CanonicalizePermissionModeForFlavor(summary.permission_mode, session.flavor);
        if (session.permission_mode.empty()) {
          session.permission_mode = policy::DefaultPermissionModeForFlavor(session.flavor);
        }
        session.model = summary.model;
        session.title_initialized = !session.name.empty();
        sessions.push_back(std::move(session));
      }
    }
  }

  {
    int opencode_exit_code = -1;
    const std::string raw_list =
        ReadCommandOutput("command -v opencode >/dev/null 2>&1 && opencode session list --format json 2>/dev/null",
                          &opencode_exit_code, {});
    if (opencode_exit_code == 0) {
      const auto parsed = ParseMaybeJson(raw_list);
      const nlohmann::json* session_list = nullptr;
      if (parsed.has_value()) {
        if (parsed->is_array()) {
          session_list = &*parsed;
        } else if (parsed->is_object()) {
          session_list = JsonField(*parsed, {"sessions", "items", "data", "results"});
        }
      }
      if (session_list != nullptr && session_list->is_array()) {
        for (const auto& entry : *session_list) {
          LocalSessionSummary summary = ReadOpencodeSessionHeader(entry, {}, 0);
          if (summary.source_session_id.empty()) {
            continue;
          }

          SessionRecord session;
          session.id = MakeExternalSessionId("opencode", summary.source_session_id);
          session.ns = "default";
          session.active = false;
          session.thinking = false;
          session.created_at_ms = summary.created_at_ms > 0 ? summary.created_at_ms : summary.updated_at_ms;
          session.updated_at_ms = summary.updated_at_ms;
          session.active_at_ms = session.updated_at_ms;
          session.thinking_at_ms = session.updated_at_ms;
          session.path = summary.cwd;
          session.transcript_path = summary.transcript_path;
          session.host = "localhost";
          session.name = summary.name;
          session.summary_text = summary.summary_text;
          session.summary_updated_at_ms = session.updated_at_ms;
          session.flavor = "opencode";
          session.source_session_id = summary.source_session_id;
          session.permission_mode = policy::CanonicalizePermissionModeForFlavor(summary.permission_mode, session.flavor);
          if (session.permission_mode.empty()) {
            session.permission_mode = policy::DefaultPermissionModeForFlavor(session.flavor);
          }
          session.model = summary.model;
          session.title_initialized = !session.name.empty();
          sessions.push_back(std::move(session));
        }
      }
    }

    const std::vector<std::filesystem::path> opencode_roots = {
        home_path / ".local" / "share" / "opencode" / "project",
        home_path / "Library" / "Application Support" / "opencode",
        home_path / "Library" / "Application Support" / "OpenCode",
    };
    for (const auto& root : opencode_roots) {
      std::error_code ec;
      if (!std::filesystem::exists(root, ec) || ec) {
        continue;
      }
      for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (ec || !it->is_regular_file()) {
          continue;
        }
        const std::filesystem::path file_path = it->path();
        if (ToLowerCopy(file_path.extension().string()) != ".json") {
          continue;
        }
        std::uintmax_t file_size = 0;
        file_size = std::filesystem::file_size(file_path, ec);
        if (ec || file_size == 0 || file_size > 5 * 1024 * 1024) {
          continue;
        }
        const auto raw_text = ReadTextFile(file_path);
        if (!raw_text.has_value()) {
          continue;
        }
        const auto parsed = ParseMaybeJson(*raw_text);
        if (!parsed.has_value() || !parsed->is_object()) {
          continue;
        }

        LocalSessionSummary summary = ReadOpencodeSessionHeader(*parsed, file_path,
                                                                FileTimeToUnixMs(std::filesystem::last_write_time(file_path, ec)));
        if (summary.source_session_id.empty()) {
          continue;
        }
        if (FindLikelyMessageArray(*parsed) == nullptr) {
          continue;
        }

        SessionRecord session;
        session.id = MakeExternalSessionId("opencode", summary.source_session_id);
        session.ns = "default";
        session.active = false;
        session.thinking = false;
        session.created_at_ms = summary.created_at_ms > 0 ? summary.created_at_ms : summary.updated_at_ms;
        session.updated_at_ms = summary.updated_at_ms;
        session.active_at_ms = session.updated_at_ms;
        session.thinking_at_ms = session.updated_at_ms;
        session.path = summary.cwd;
        session.transcript_path = summary.transcript_path;
        session.host = "localhost";
        session.name = summary.name;
        session.summary_text = summary.summary_text;
        session.summary_updated_at_ms = session.updated_at_ms;
        session.flavor = "opencode";
        session.source_session_id = summary.source_session_id;
        session.permission_mode = policy::CanonicalizePermissionModeForFlavor(summary.permission_mode, session.flavor);
        if (session.permission_mode.empty()) {
          session.permission_mode = policy::DefaultPermissionModeForFlavor(session.flavor);
        }
        session.model = summary.model;
        session.title_initialized = !session.name.empty();
        sessions.push_back(std::move(session));
      }
    }
  }

  std::unordered_map<std::string, SessionRecord> deduped;
  for (auto& session : sessions) {
    auto it = deduped.find(session.id);
    if (it == deduped.end() || session.updated_at_ms > it->second.updated_at_ms) {
      deduped[session.id] = std::move(session);
    }
  }

  sessions.clear();
  sessions.reserve(deduped.size());
  for (auto& [_, session] : deduped) {
    sessions.push_back(std::move(session));
  }
  std::sort(sessions.begin(), sessions.end(), [](const SessionRecord& lhs, const SessionRecord& rhs) {
    return lhs.updated_at_ms > rhs.updated_at_ms;
  });
  return sessions;
}

std::optional<CodeAgentManager::SessionRecord> CodeAgentManager::FindExternalSession(const std::string& ns,
                                                                                      const std::string& session_id) const {
  const std::string normalized_session_id = util::Trim(session_id);
  if (normalized_session_id.empty()) {
    return std::nullopt;
  }
  for (auto& session : DiscoverExternalSessions(ns)) {
    if (session.id == normalized_session_id) {
      return session;
    }
  }
  return std::nullopt;
}

bool CodeAgentManager::PopulateExternalSessionMessages(SessionRecord* session, std::string* error) const {
  if (session == nullptr) {
    if (error != nullptr) {
      *error = "invalid session";
    }
    return false;
  }
  const auto finalize_loaded_session = [&](const std::string& latest_preview_text) {
    if (session->name.empty()) {
      if (!latest_preview_text.empty()) {
        session->name = TruncateText(latest_preview_text, 80);
      } else if (!session->path.empty()) {
        session->name = BasenameForDisplay(session->path);
      } else {
        session->name = session->source_session_id;
      }
    }
    if (session->summary_text.empty()) {
      session->summary_text = latest_preview_text.empty() ? session->name : TruncateText(latest_preview_text, 120);
    }
    if (session->summary_updated_at_ms <= 0) {
      session->summary_updated_at_ms = session->updated_at_ms;
    }
    if (!session->messages.empty()) {
      session->created_at_ms = std::min(session->created_at_ms > 0 ? session->created_at_ms : session->messages.front().created_at_ms,
                                        session->messages.front().created_at_ms);
      session->updated_at_ms = std::max(session->updated_at_ms, session->messages.back().created_at_ms);
      session->seq = static_cast<int>(session->messages.size() + 1);
    }
    if (error != nullptr) {
      error->clear();
    }
  };

  if (session->flavor == "cursor") {
    if (session->transcript_path.empty()) {
      if (error != nullptr) {
        *error = "session transcript not found";
      }
      return false;
    }

    auto db = OpenReadOnlySqlite(session->transcript_path);
    if (!db) {
      if (error != nullptr) {
        *error = "failed to open cursor session database";
      }
      return false;
    }
    const auto raw_sessions = ReadSqliteItemValue(db.get(), "interactive.sessions");
    if (!raw_sessions.has_value()) {
      if (error != nullptr) {
        *error = "cursor interactive sessions not found";
      }
      return false;
    }
    const auto parsed = ParseMaybeJson(*raw_sessions);
    if (!parsed.has_value() || !parsed->is_array()) {
      if (error != nullptr) {
        *error = "invalid cursor interactive session payload";
      }
      return false;
    }

    session->messages.clear();
    session->seq = 1;
    std::string latest_preview_text;
    const auto append_message = [&](std::int64_t created_at_ms, nlohmann::json content) {
      MessageRecord message;
      message.id = session->id + "-msg-" + std::to_string(session->messages.size() + 1);
      message.seq = static_cast<int>(session->messages.size() + 1);
      message.created_at_ms = created_at_ms > 0 ? created_at_ms : std::max<std::int64_t>(session->updated_at_ms, session->created_at_ms);
      message.content = std::move(content);
      session->messages.push_back(std::move(message));
    };

    const std::filesystem::path workspace_path = ReadCursorWorkspacePath(session->transcript_path.parent_path());
    if (session->path.empty()) {
      session->path = workspace_path;
    }

    bool found = false;
    for (const auto& session_json : *parsed) {
      if (!session_json.is_object()) {
        continue;
      }
      const auto source_session_id = JsonStringValue(session_json, {"sessionId", "id"});
      if (!source_session_id.has_value() || *source_session_id != session->source_session_id) {
        continue;
      }
      found = true;
      if (session->name.empty()) {
        if (auto custom_title = JsonStringValue(session_json, {"customTitle", "title", "name"}); custom_title.has_value()) {
          session->name = *custom_title;
          session->title_initialized = true;
        }
      }
      if (session->created_at_ms <= 0) {
        session->created_at_ms = JsonTimestampValue(session_json, {"creationDate", "createdAt", "startTime"});
      }
      session->updated_at_ms = std::max(session->updated_at_ms,
                                        JsonTimestampValue(session_json, {"lastMessageDate", "lastUpdated", "updatedAt"}));

      if (const auto* requests = JsonField(session_json, {"requests"}); requests != nullptr && requests->is_array()) {
        for (const auto& request : *requests) {
          const std::int64_t timestamp_ms = JsonTimestampValue(request, {"timestamp", "createdAt", "updatedAt"});
          const std::string user_text = ExtractCursorUserText(request);
          if (!user_text.empty()) {
            append_message(timestamp_ms, WrapUserTextMessage(user_text));
            latest_preview_text = user_text;
          }
          const std::string assistant_text = ExtractCursorAssistantText(request);
          if (!assistant_text.empty()) {
            append_message(timestamp_ms, WrapCodexBodyMessage({{"type", "message"}, {"message", assistant_text}}));
            latest_preview_text = assistant_text;
          }
        }
      }
      break;
    }

    if (!found) {
      if (error != nullptr) {
        *error = "cursor session not found";
      }
      return false;
    }

    finalize_loaded_session(latest_preview_text);
    return true;
  }

  if (session->flavor == "gemini") {
    if (session->transcript_path.empty()) {
      if (error != nullptr) {
        *error = "session transcript not found";
      }
      return false;
    }
    const auto raw_text = ReadTextFile(session->transcript_path);
    if (!raw_text.has_value()) {
      if (error != nullptr) {
        *error = "failed to open gemini session transcript";
      }
      return false;
    }
    const auto parsed = ParseMaybeJson(*raw_text);
    if (!parsed.has_value() || !parsed->is_object()) {
      if (error != nullptr) {
        *error = "invalid gemini session transcript";
      }
      return false;
    }

    session->messages.clear();
    session->seq = 1;
    std::string latest_preview_text;
    const auto append_message = [&](std::int64_t created_at_ms, nlohmann::json content) {
      MessageRecord message;
      message.id = session->id + "-msg-" + std::to_string(session->messages.size() + 1);
      message.seq = static_cast<int>(session->messages.size() + 1);
      message.created_at_ms = created_at_ms > 0 ? created_at_ms : std::max<std::int64_t>(session->updated_at_ms, session->created_at_ms);
      message.content = std::move(content);
      session->messages.push_back(std::move(message));
    };

    const nlohmann::json& root = *parsed;
    if (session->source_session_id.empty()) {
      if (auto source_session_id = JsonStringValue(root, {"sessionId", "id"}); source_session_id.has_value()) {
        session->source_session_id = *source_session_id;
      }
    }
    if (session->created_at_ms <= 0) {
      session->created_at_ms = JsonTimestampValue(root, {"startTime", "createdAt", "created_at"});
    }
    session->updated_at_ms = std::max(session->updated_at_ms,
                                      JsonTimestampValue(root, {"lastUpdated", "updatedAt", "updated_at"}));
    if (session->name.empty()) {
      if (auto summary = JsonStringValue(root, {"summary", "title", "name"}); summary.has_value()) {
        session->name = TruncateText(*summary, 80);
        session->title_initialized = true;
      }
    }

    const auto* messages = JsonField(root, {"messages"});
    if (messages == nullptr || !messages->is_array()) {
      if (error != nullptr) {
        *error = "gemini session messages not found";
      }
      return false;
    }

    for (const auto& message : *messages) {
      if (!message.is_object()) {
        continue;
      }
      const std::int64_t timestamp_ms = JsonTimestampValue(message, {"timestamp", "createdAt", "updatedAt"});
      const std::string type = ToLowerCopy(message.value("type", std::string()));
      if (type == "user") {
        const std::string text = ExtractGeminiContentText(message.value("content", nlohmann::json(nullptr)));
        if (!text.empty()) {
          append_message(timestamp_ms, WrapUserTextMessage(text));
          latest_preview_text = text;
        }
        continue;
      }
      if (type != "gemini") {
        continue;
      }

      if (session->model.empty()) {
        if (auto model = JsonStringValue(message, {"model"}); model.has_value()) {
          session->model = *model;
        }
      }

      const auto* content = JsonField(message, {"content"});
      if (content != nullptr && content->is_array()) {
        for (const auto& part : *content) {
          if (!part.is_object()) {
            continue;
          }
          auto function_call_it = part.find("functionCall");
          if (function_call_it != part.end() && function_call_it->is_object()) {
            const nlohmann::json& call = *function_call_it;
            append_message(timestamp_ms,
                           WrapCodexBodyMessage({
                               {"type", "tool-call"},
                               {"name", call.value("name", std::string("Tool"))},
                               {"callId", call.value("id", std::string())},
                               {"input", call.value("args", nlohmann::json::object())},
                           }));
          }
          auto function_response_it = part.find("functionResponse");
          if (function_response_it != part.end() && function_response_it->is_object()) {
            const nlohmann::json& response = *function_response_it;
            append_message(timestamp_ms,
                           WrapCodexBodyMessage({
                               {"type", "tool-call-result"},
                               {"callId", response.value("id", std::string())},
                               {"output", response.value("response", nlohmann::json::object())},
                               {"is_error", response.value("error", false)},
                           }));
          }
        }
      }
      if (const auto* tool_calls = JsonField(message, {"toolCalls"}); tool_calls != nullptr && tool_calls->is_array()) {
        for (const auto& call : *tool_calls) {
          if (!call.is_object()) {
            continue;
          }
          const std::string call_id = call.value("id", std::string());
          nlohmann::json input_value = call.value("args", call.value("arguments", call.value("input", nlohmann::json::object())));
          append_message(timestamp_ms,
                         WrapCodexBodyMessage({
                             {"type", "tool-call"},
                             {"name", call.value("name", call.value("displayName", std::string("Tool")))},
                             {"callId", call_id},
                             {"input", input_value},
                         }));
          const auto* output = JsonField(call, {"response", "result", "output"});
          if (output != nullptr && !output->is_null()) {
            append_message(timestamp_ms,
                           WrapCodexBodyMessage({
                               {"type", "tool-call-result"},
                               {"callId", call_id},
                               {"output", *output},
                               {"is_error", false},
                           }));
          }
        }
      }

      const std::string text = ExtractGeminiContentText(message.value("content", nlohmann::json(nullptr)));
      if (!text.empty()) {
        append_message(timestamp_ms, WrapCodexBodyMessage({{"type", "message"}, {"message", text}}));
        latest_preview_text = text;
      }
    }

    finalize_loaded_session(latest_preview_text);
    return true;
  }

  if (session->flavor == "opencode") {
    std::optional<nlohmann::json> parsed;
    if (!session->source_session_id.empty()) {
      int export_exit_code = -1;
      const std::string command =
          "command -v opencode >/dev/null 2>&1 && opencode export " + ShellEscape(session->source_session_id) + " 2>/dev/null";
      const std::string raw_export = ReadCommandOutput(command, &export_exit_code, {});
      if (export_exit_code == 0) {
        parsed = ParseMaybeJson(raw_export);
      }
      if (!parsed.has_value()) {
        export_exit_code = -1;
        const std::string raw_export_json =
            ReadCommandOutput("command -v opencode >/dev/null 2>&1 && opencode export " +
                                  ShellEscape(session->source_session_id) + " --format json 2>/dev/null",
                              &export_exit_code, {});
        if (export_exit_code == 0) {
          parsed = ParseMaybeJson(raw_export_json);
        }
      }
    }
    if (!parsed.has_value() && !session->transcript_path.empty()) {
      if (const auto raw_text = ReadTextFile(session->transcript_path); raw_text.has_value()) {
        parsed = ParseMaybeJson(*raw_text);
      }
    }
    if (!parsed.has_value()) {
      if (error != nullptr) {
        *error = "failed to load opencode session transcript";
      }
      return false;
    }

    const nlohmann::json* messages = FindLikelyMessageArray(*parsed);
    if (messages == nullptr || !messages->is_array()) {
      if (error != nullptr) {
        *error = "opencode session messages not found";
      }
      return false;
    }

    session->messages.clear();
    session->seq = 1;
    std::string latest_preview_text;
    const auto append_message = [&](std::int64_t created_at_ms, nlohmann::json content) {
      MessageRecord message;
      message.id = session->id + "-msg-" + std::to_string(session->messages.size() + 1);
      message.seq = static_cast<int>(session->messages.size() + 1);
      message.created_at_ms = created_at_ms > 0 ? created_at_ms : std::max<std::int64_t>(session->updated_at_ms, session->created_at_ms);
      message.content = std::move(content);
      session->messages.push_back(std::move(message));
    };

    if (session->source_session_id.empty()) {
      if (auto source_session_id = JsonStringValue(*parsed, {"sessionId", "session_id", "id", "uuid"}); source_session_id.has_value()) {
        session->source_session_id = *source_session_id;
      }
    }
    if (session->name.empty()) {
      if (auto name = JsonStringValue(*parsed, {"title", "name", "summary"}); name.has_value()) {
        session->name = TruncateText(*name, 80);
        session->title_initialized = true;
      }
    }
    if (session->path.empty()) {
      if (auto cwd = JsonStringValue(*parsed, {"cwd", "path", "projectPath"}); cwd.has_value()) {
        session->path = FileUriToPath(*cwd);
      } else if (const auto* project = JsonField(*parsed, {"project"}); project != nullptr && project->is_object()) {
        if (auto cwd = JsonStringValue(*project, {"path", "cwd", "root"}); cwd.has_value()) {
          session->path = FileUriToPath(*cwd);
        }
      }
    }
    if (session->created_at_ms <= 0) {
      session->created_at_ms = JsonTimestampValue(*parsed, {"startTime", "createdAt", "created_at", "timestamp"});
    }
    session->updated_at_ms = std::max(session->updated_at_ms,
                                      JsonTimestampValue(*parsed, {"lastUpdated", "updatedAt", "updated_at", "timestamp"}));

    for (const auto& message : *messages) {
      const std::int64_t timestamp_ms = JsonTimestampValue(message, {"timestamp", "createdAt", "updatedAt", "time"});
      if (message.is_object()) {
        if (const auto* tool_call = JsonField(message, {"toolCall", "functionCall"}); tool_call != nullptr && tool_call->is_object()) {
          append_message(timestamp_ms,
                         WrapCodexBodyMessage({
                             {"type", "tool-call"},
                             {"name", tool_call->value("name", std::string("Tool"))},
                             {"callId", tool_call->value("id", std::string())},
                             {"input", tool_call->value("args", tool_call->value("input", nlohmann::json::object()))},
                         }));
        }
        if (const auto* tool_result = JsonField(message, {"toolResult", "functionResponse"}); tool_result != nullptr && tool_result->is_object()) {
          append_message(timestamp_ms,
                         WrapCodexBodyMessage({
                             {"type", "tool-call-result"},
                             {"callId", tool_result->value("id", std::string())},
                             {"output", tool_result->value("response", tool_result->value("result", tool_result->value("output", nlohmann::json::object())))},
                             {"is_error", tool_result->value("error", false)},
                         }));
        }
        if (const auto* tool_calls = JsonField(message, {"toolCalls"}); tool_calls != nullptr && tool_calls->is_array()) {
          for (const auto& call : *tool_calls) {
            if (!call.is_object()) {
              continue;
            }
            const std::string call_id = call.value("id", std::string());
            append_message(timestamp_ms,
                           WrapCodexBodyMessage({
                               {"type", "tool-call"},
                               {"name", call.value("name", call.value("displayName", std::string("Tool")))},
                               {"callId", call_id},
                               {"input", call.value("args", call.value("input", nlohmann::json::object()))},
                           }));
            const auto* output = JsonField(call, {"response", "result", "output"});
            if (output != nullptr && !output->is_null()) {
              append_message(timestamp_ms,
                             WrapCodexBodyMessage({
                                 {"type", "tool-call-result"},
                                 {"callId", call_id},
                                 {"output", *output},
                                 {"is_error", false},
                             }));
            }
          }
        }
      }

      const std::string role = message.is_object()
                                   ? ToLowerCopy(message.value("role", message.value("sender", message.value("type", std::string()))))
                                   : std::string();
      const std::string text = ExtractOpencodeMessageText(message);
      if ((role == "user" || role == "human") && !text.empty()) {
        append_message(timestamp_ms, WrapUserTextMessage(text));
        latest_preview_text = text;
        continue;
      }
      if ((role == "assistant" || role == "agent" || role == "model" || role == "opencode") && !text.empty()) {
        append_message(timestamp_ms, WrapCodexBodyMessage({{"type", "message"}, {"message", text}}));
        latest_preview_text = text;
      }
    }

    finalize_loaded_session(latest_preview_text);
    return true;
  }

  if (session->transcript_path.empty()) {
    if (error != nullptr) {
      *error = "session transcript not found";
    }
    return false;
  }

  std::ifstream input(session->transcript_path);
  if (!input.is_open()) {
    if (error != nullptr) {
      *error = "failed to open session transcript";
    }
    return false;
  }

  session->messages.clear();
  session->seq = 1;
  std::string latest_preview_text;

  const auto append_message = [&](std::int64_t created_at_ms, nlohmann::json content) {
    MessageRecord message;
    message.id = session->id + "-msg-" + std::to_string(session->messages.size() + 1);
    message.seq = static_cast<int>(session->messages.size() + 1);
    message.created_at_ms = created_at_ms > 0 ? created_at_ms : std::max<std::int64_t>(session->updated_at_ms, session->created_at_ms);
    message.content = std::move(content);
    session->messages.push_back(std::move(message));
  };

  std::string line;
  while (std::getline(input, line)) {
    auto parsed = ParseJsonLine(line);
    if (!parsed.has_value() || !parsed->is_object()) {
      continue;
    }

    const std::int64_t timestamp_ms = [&]() -> std::int64_t {
      if (auto timestamp = JsonStringValue(*parsed, {"timestamp"}); timestamp.has_value()) {
        return ParseIso8601ToUnixMs(*timestamp);
      }
      return 0;
    }();
    if (timestamp_ms > 0) {
      if (session->created_at_ms <= 0) {
        session->created_at_ms = timestamp_ms;
      }
      session->updated_at_ms = std::max(session->updated_at_ms, timestamp_ms);
      if (session->active_at_ms <= 0) {
        session->active_at_ms = timestamp_ms;
      }
      session->thinking_at_ms = session->updated_at_ms;
    }

    if (session->flavor == "codex") {
      const std::string record_type = parsed->value("type", std::string());
      if (record_type == "session_meta") {
        const nlohmann::json payload = parsed->value("payload", nlohmann::json::object());
        if (session->source_session_id.empty()) {
          if (auto source_session_id = JsonStringValue(payload, {"id"}); source_session_id.has_value()) {
            session->source_session_id = *source_session_id;
          }
        }
        if (session->path.empty()) {
          if (auto cwd = JsonStringValue(payload, {"cwd"}); cwd.has_value()) {
            session->path = std::filesystem::path(*cwd);
          }
        }
        continue;
      }

      if (record_type == "turn_context") {
        const nlohmann::json payload = parsed->value("payload", nlohmann::json::object());
        if (session->model.empty()) {
          if (auto model = JsonStringValue(payload, {"model"}); model.has_value()) {
            session->model = *model;
          }
        }
        if (auto effort = JsonStringValue(payload, {"effort", "reasoning_effort", "reasoningEffort"}); effort.has_value()) {
          const std::string normalized = policy::NormalizeReasoningEffortValue(*effort);
          if (policy::IsReasoningEffortAllowedForFlavor(normalized, session->flavor)) {
            session->model_reasoning_effort = normalized.empty() ? std::string("medium") : normalized;
          }
        }
        continue;
      }

      if (record_type == "event_msg") {
        const nlohmann::json payload = parsed->value("payload", nlohmann::json::object());
        if (payload.value("type", std::string()) == "user_message") {
          const std::string text = util::Trim(payload.value("message", std::string()));
          if (!text.empty()) {
            append_message(timestamp_ms, WrapUserTextMessage(text));
            latest_preview_text = text;
          }
        }
        continue;
      }

      if (record_type != "response_item") {
        continue;
      }

      const nlohmann::json payload = parsed->value("payload", nlohmann::json::object());
      const std::string payload_type = payload.value("type", std::string());
      if (payload_type == "message" && payload.value("role", std::string()) == "assistant") {
        const std::string text = JoinCodexAssistantText(payload.value("content", nlohmann::json::array()));
        if (!text.empty()) {
          append_message(timestamp_ms, WrapCodexBodyMessage({{"type", "message"}, {"message", text}}));
          latest_preview_text = text;
        }
        continue;
      }
      if (payload_type == "function_call") {
        nlohmann::json input_value = payload.value("arguments", nlohmann::json(nullptr));
        if (input_value.is_string()) {
          const auto parsed_input = ParseMaybeJson(input_value.get<std::string>());
          if (parsed_input.has_value()) {
            input_value = *parsed_input;
          }
        }
        append_message(timestamp_ms,
                       WrapCodexBodyMessage({
                           {"type", "tool-call"},
                           {"name", payload.value("name", std::string("Tool"))},
                           {"callId", payload.value("call_id", std::string())},
                           {"input", input_value},
                       }));
        continue;
      }
      if (payload_type == "function_call_output") {
        nlohmann::json output_value = payload.value("output", nlohmann::json(nullptr));
        if (output_value.is_string()) {
          const auto parsed_output = ParseMaybeJson(output_value.get<std::string>());
          if (parsed_output.has_value()) {
            output_value = *parsed_output;
          }
        }
        append_message(timestamp_ms,
                       WrapCodexBodyMessage({
                           {"type", "tool-call-result"},
                           {"callId", payload.value("call_id", std::string())},
                           {"output", output_value},
                           {"is_error", false},
                       }));
        continue;
      }
      continue;
    }

    const std::string record_type = parsed->value("type", std::string());
    if (record_type == "user") {
      if (session->path.empty()) {
        if (auto cwd = JsonStringValue(*parsed, {"cwd"}); cwd.has_value()) {
          session->path = std::filesystem::path(*cwd);
        }
      }
      if (auto permission_mode = JsonStringValue(*parsed, {"permissionMode"}); permission_mode.has_value()) {
        const std::string normalized = policy::CanonicalizePermissionModeForFlavor(*permission_mode, session->flavor);
        if (!normalized.empty()) {
          session->permission_mode = normalized;
        }
      }

      const nlohmann::json message = parsed->value("message", nlohmann::json::object());
      const auto content_it = message.find("content");
      if (content_it == message.end()) {
        continue;
      }
      if (content_it->is_string()) {
        const std::string text = util::Trim(content_it->get<std::string>());
        if (!text.empty()) {
          append_message(timestamp_ms, WrapUserTextMessage(text));
          latest_preview_text = text;
        }
        continue;
      }
      if (!content_it->is_array()) {
        continue;
      }

      std::string combined_user_text;
      for (const auto& block : *content_it) {
        if (!block.is_object()) {
          continue;
        }
        const std::string block_type = block.value("type", std::string());
        if (block_type == "text") {
          const std::string text = util::Trim(block.value("text", std::string()));
          if (!text.empty()) {
            if (!combined_user_text.empty()) {
              combined_user_text += "\n\n";
            }
            combined_user_text += text;
          }
          continue;
        }
        if (block_type != "tool_result") {
          continue;
        }
        nlohmann::json output_value = block.value("content", nlohmann::json(nullptr));
        append_message(timestamp_ms,
                       WrapCodexBodyMessage({
                           {"type", "tool-call-result"},
                           {"callId", block.value("tool_use_id", std::string())},
                           {"output", output_value},
                           {"is_error", block.value("is_error", false)},
                       }));
      }
      if (!combined_user_text.empty()) {
        append_message(timestamp_ms, WrapUserTextMessage(combined_user_text));
        latest_preview_text = combined_user_text;
      }
      continue;
    }

    if (record_type != "assistant") {
      continue;
    }

    const nlohmann::json message = parsed->value("message", nlohmann::json::object());
    if (session->model.empty()) {
      if (auto model = JsonStringValue(message, {"model"}); model.has_value()) {
        session->model = *model;
      }
    }
    const auto content_it = message.find("content");
    if (content_it == message.end()) {
      continue;
    }
    if (content_it->is_string()) {
      const std::string text = util::Trim(content_it->get<std::string>());
      if (!text.empty()) {
        append_message(timestamp_ms, WrapCodexBodyMessage({{"type", "message"}, {"message", text}}));
        latest_preview_text = text;
      }
      continue;
    }
    if (!content_it->is_array()) {
      continue;
    }

    std::string combined_assistant_text;
    const auto flush_assistant_text = [&]() {
      if (combined_assistant_text.empty()) {
        return;
      }
      append_message(timestamp_ms,
                     WrapCodexBodyMessage({{"type", "message"}, {"message", combined_assistant_text}}));
      latest_preview_text = combined_assistant_text;
      combined_assistant_text.clear();
    };

    for (const auto& block : *content_it) {
      if (!block.is_object()) {
        continue;
      }
      const std::string block_type = block.value("type", std::string());
      if (block_type == "text") {
        const std::string text = util::Trim(block.value("text", std::string()));
        if (!text.empty()) {
          if (!combined_assistant_text.empty()) {
            combined_assistant_text += "\n\n";
          }
          combined_assistant_text += text;
        }
        continue;
      }
      if (block_type == "tool_use") {
        flush_assistant_text();
        const std::string tool_name = block.value("name", std::string("Tool"));
        const nlohmann::json input_value = block.value("input", nlohmann::json::object());
        if (session->name.empty()) {
          if (auto title = ExtractTitleFromToolUse(tool_name, input_value); title.has_value()) {
            session->name = *title;
            session->title_initialized = true;
          }
        }
        append_message(timestamp_ms,
                       WrapCodexBodyMessage({
                           {"type", "tool-call"},
                           {"name", tool_name},
                           {"callId", block.value("id", std::string())},
                           {"input", input_value},
                       }));
      }
    }
    flush_assistant_text();
  }

  if (session->name.empty()) {
    if (!latest_preview_text.empty()) {
      session->name = TruncateText(latest_preview_text, 80);
    } else if (!session->path.empty()) {
      session->name = BasenameForDisplay(session->path);
    } else {
      session->name = session->source_session_id;
    }
  }
  if (session->summary_text.empty()) {
    session->summary_text = latest_preview_text.empty() ? session->name : TruncateText(latest_preview_text, 120);
  }
  if (session->summary_updated_at_ms <= 0) {
    session->summary_updated_at_ms = session->updated_at_ms;
  }
  if (!session->messages.empty()) {
    session->created_at_ms = std::min(session->created_at_ms > 0 ? session->created_at_ms : session->messages.front().created_at_ms,
                                      session->messages.front().created_at_ms);
    session->updated_at_ms = std::max(session->updated_at_ms, session->messages.back().created_at_ms);
    session->seq = static_cast<int>(session->messages.size() + 1);
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool CodeAgentManager::ImportExternalSessionLocked(const std::string& ns, SessionRecord session, std::string* error) {
  auto existing = sessions_by_id_.find(session.id);
  if (existing != sessions_by_id_.end()) {
    existing->second.active = true;
    existing->second.active_at_ms = NowMs();
    existing->second.updated_at_ms = existing->second.active_at_ms;
    PushEventLocked(ns,
                    {
                        {"type", "session-updated"},
                        {"namespace", ns},
                        {"sessionId", existing->second.id},
                        {"data", BuildSessionJsonLocked(existing->second)},
                    },
                    existing->second.id);
    PersistStateLocked();
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }

  session.ns = ns;
  session.active = true;
  session.thinking = false;
  session.active_at_ms = NowMs();
  session.updated_at_ms = std::max(session.updated_at_ms, session.active_at_ms);
  session.thinking_at_ms = session.updated_at_ms;
  session.machine_id = machine_.id;
  if (session.permission_mode.empty()) {
    session.permission_mode = policy::DefaultPermissionModeForFlavor(session.flavor);
  }
  if (!policy::IsModelModeAllowedForFlavor(session.model_mode, session.flavor)) {
    session.model_mode = "default";
  }
  if (!policy::IsReasoningEffortAllowedForFlavor(session.model_reasoning_effort, session.flavor)) {
    session.model_reasoning_effort.clear();
  } else if (session.flavor == "codex" && session.model_reasoning_effort.empty()) {
    session.model_reasoning_effort = "medium";
  }
  if (session.flavor != "codex") {
    session.codex_fast = false;
  }

  session_ids_by_ns_[ns].push_back(session.id);
  SessionRecord& stored = sessions_by_id_.insert_or_assign(session.id, std::move(session)).first->second;
  PushEventLocked(ns,
                  {
                      {"type", "session-updated"},
                      {"namespace", ns},
                      {"sessionId", stored.id},
                      {"data", BuildSessionJsonLocked(stored)},
                  },
                  stored.id);
  PersistStateLocked();
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

}  // namespace ferryman::codeagent
