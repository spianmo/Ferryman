#include "ferryman/codeagent/CodeAgentManager.hpp"

#include "CodeAgentPolicy.hpp"
#include "ferryman/util/Random.hpp"
#include "ferryman/util/StringUtil.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sqlite3.h>
#include <string>
#include <system_error>

namespace ferryman::codeagent {

namespace {

constexpr int kStateSchemaVersion = 1;

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

enum class SqliteStateLoadStatus {
  kLoaded,
  kMissing,
  kError,
};

using SqliteDbPtr = std::unique_ptr<sqlite3, SqliteDbCloser>;
using SqliteStmtPtr = std::unique_ptr<sqlite3_stmt, SqliteStmtCloser>;

SqliteDbPtr OpenStateDatabase(const std::filesystem::path& db_path) {
  if (db_path.empty()) {
    return nullptr;
  }

  const std::filesystem::path parent = db_path.parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return nullptr;
    }
  }

  sqlite3* db = nullptr;
  if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return nullptr;
  }
  return SqliteDbPtr(db);
}

bool EnsureStateTable(sqlite3* db) {
  if (db == nullptr) {
    return false;
  }
  const char* sql =
      "CREATE TABLE IF NOT EXISTS codeagent_state ("
      "id INTEGER PRIMARY KEY CHECK(id = 1),"
      "version INTEGER NOT NULL,"
      "payload_json TEXT NOT NULL,"
      "updated_at_ms INTEGER NOT NULL"
      ")";
  return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

SqliteStateLoadStatus LoadStatePayloadFromSqlite(const std::filesystem::path& db_path, std::string* payload_json) {
  if (payload_json == nullptr) {
    return SqliteStateLoadStatus::kError;
  }
  payload_json->clear();

  auto db = OpenStateDatabase(db_path);
  if (!db) {
    return SqliteStateLoadStatus::kError;
  }
  if (!EnsureStateTable(db.get())) {
    return SqliteStateLoadStatus::kError;
  }

  sqlite3_stmt* raw_stmt = nullptr;
  const char* sql = "SELECT payload_json FROM codeagent_state WHERE id = 1 LIMIT 1";
  if (sqlite3_prepare_v2(db.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) {
    return SqliteStateLoadStatus::kError;
  }
  SqliteStmtPtr stmt(raw_stmt);

  const int rc = sqlite3_step(stmt.get());
  if (rc == SQLITE_ROW) {
    const unsigned char* text = sqlite3_column_text(stmt.get(), 0);
    *payload_json = text == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(text));
    return SqliteStateLoadStatus::kLoaded;
  }
  if (rc == SQLITE_DONE) {
    return SqliteStateLoadStatus::kMissing;
  }
  return SqliteStateLoadStatus::kError;
}

bool PersistStatePayloadToSqlite(const std::filesystem::path& db_path, const std::string& payload_json,
                                 std::int64_t updated_at_ms) {
  auto db = OpenStateDatabase(db_path);
  if (!db) {
    return false;
  }
  if (!EnsureStateTable(db.get())) {
    return false;
  }

  sqlite3_stmt* raw_stmt = nullptr;
  const char* sql =
      "INSERT OR REPLACE INTO codeagent_state(id, version, payload_json, updated_at_ms) "
      "VALUES (1, ?, ?, ?)";
  if (sqlite3_prepare_v2(db.get(), sql, -1, &raw_stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  SqliteStmtPtr stmt(raw_stmt);

  if (sqlite3_bind_int(stmt.get(), 1, kStateSchemaVersion) != SQLITE_OK) {
    return false;
  }
  if (sqlite3_bind_text(stmt.get(), 2, payload_json.c_str(), static_cast<int>(payload_json.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    return false;
  }
  if (sqlite3_bind_int64(stmt.get(), 3, static_cast<sqlite3_int64>(updated_at_ms)) != SQLITE_OK) {
    return false;
  }

  return sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool LoadJsonFromFile(const std::filesystem::path& file_path, nlohmann::json* root) {
  if (root == nullptr || file_path.empty()) {
    return false;
  }
  std::ifstream input(file_path);
  if (!input.is_open()) {
    return false;
  }
  try {
    input >> *root;
  } catch (...) {
    return false;
  }
  return root->is_object();
}

}  // namespace

void CodeAgentManager::RestoreStateLocked() {
  if (state_file_path_.empty()) {
    return;
  }

  nlohmann::json root;
  bool loaded_from_legacy_json = false;

  std::string sqlite_payload;
  if (LoadStatePayloadFromSqlite(state_file_path_, &sqlite_payload) == SqliteStateLoadStatus::kLoaded &&
      !sqlite_payload.empty()) {
    try {
      root = nlohmann::json::parse(sqlite_payload);
    } catch (...) {
      root = nlohmann::json();
    }
  }

  if (!root.is_object() && !legacy_state_file_path_.empty()) {
    nlohmann::json legacy_root;
    if (LoadJsonFromFile(legacy_state_file_path_, &legacy_root)) {
      root = std::move(legacy_root);
      loaded_from_legacy_json = true;
    }
  }

  if (!root.is_object()) {
    return;
  }

  const auto parse_int64 = [](const nlohmann::json& value, std::int64_t fallback) -> std::int64_t {
    if (value.is_number_integer()) {
      return value.get<std::int64_t>();
    }
    if (value.is_number_unsigned()) {
      return static_cast<std::int64_t>(value.get<std::uint64_t>());
    }
    if (value.is_number_float()) {
      return static_cast<std::int64_t>(value.get<double>());
    }
    return fallback;
  };

  const auto parse_bool = [](const nlohmann::json& value, bool fallback) -> bool {
    return value.is_boolean() ? value.get<bool>() : fallback;
  };

  const auto parse_string = [](const nlohmann::json& value, const std::string& fallback) -> std::string {
    return value.is_string() ? value.get<std::string>() : fallback;
  };

  const auto machine_it = root.find("machine");
  if (machine_it != root.end() && machine_it->is_object()) {
    const nlohmann::json& machine = *machine_it;
    const std::string restored_machine_id = parse_string(machine.value("id", nlohmann::json(nullptr)), "");
    if (!restored_machine_id.empty()) {
      machine_.id = restored_machine_id;
    }
    machine_.active = parse_bool(machine.value("active", nlohmann::json(nullptr)), machine_.active);
    if (machine.contains("metadata")) {
      machine_.metadata = machine["metadata"];
    }
    machine_.updated_at_ms = parse_int64(machine.value("updatedAt", nlohmann::json(nullptr)), machine_.updated_at_ms);
  }

  sessions_by_id_.clear();
  session_ids_by_ns_.clear();

  const auto sessions_it = root.find("sessions");
  if (sessions_it == root.end() || !sessions_it->is_array()) {
    if (loaded_from_legacy_json) {
      PersistStateLocked();
    }
    return;
  }

  for (const auto& raw_session : *sessions_it) {
    if (!raw_session.is_object()) {
      continue;
    }

    SessionRecord session;
    session.id = parse_string(raw_session.value("id", nlohmann::json(nullptr)), "");
    if (session.id.empty()) {
      continue;
    }
    session.ns = util::Trim(parse_string(raw_session.value("ns", nlohmann::json("default")), "default"));
    if (session.ns.empty()) {
      session.ns = "default";
    }
    session.seq = std::max(1, static_cast<int>(parse_int64(raw_session.value("seq", nlohmann::json(1)), 1)));
    session.created_at_ms = parse_int64(raw_session.value("createdAt", nlohmann::json(nullptr)), NowMs());
    session.updated_at_ms = parse_int64(raw_session.value("updatedAt", nlohmann::json(nullptr)), session.created_at_ms);
    session.active = parse_bool(raw_session.value("active", nlohmann::json(nullptr)), true);
    session.active_at_ms = parse_int64(raw_session.value("activeAt", nlohmann::json(nullptr)), session.created_at_ms);
    session.thinking = false;
    session.thinking_at_ms = parse_int64(raw_session.value("thinkingAt", nlohmann::json(nullptr)), session.updated_at_ms);
    session.path = std::filesystem::path(parse_string(raw_session.value("path", nlohmann::json("")), ""));
    session.host = parse_string(raw_session.value("host", nlohmann::json("localhost")), "localhost");
    session.name = parse_string(raw_session.value("name", nlohmann::json("")), "");
    if (session.name.empty() && !session.path.empty()) {
      session.name = session.path.filename().string();
    }
    session.summary_text = parse_string(raw_session.value("summaryText", nlohmann::json("")), "");
    session.summary_updated_at_ms =
        parse_int64(raw_session.value("summaryUpdatedAt", nlohmann::json(nullptr)), session.updated_at_ms);
    session.flavor = NormalizeAgent(parse_string(raw_session.value("flavor", nlohmann::json("claude")), "claude"));
    session.machine_id = parse_string(raw_session.value("machineId", nlohmann::json(machine_.id)), machine_.id);
    session.permission_mode =
        parse_string(raw_session.value("permissionMode", nlohmann::json("default")), "default");
    session.model_mode = parse_string(raw_session.value("modelMode", nlohmann::json("default")), "default");
    session.model_reasoning_effort =
        parse_string(raw_session.value("reasoningEffort", nlohmann::json("")), "");
    if (session.model_reasoning_effort.empty()) {
      session.model_reasoning_effort =
          parse_string(raw_session.value("modelReasoningEffort", nlohmann::json("")), "");
    }
    if (session.model_reasoning_effort.empty()) {
      session.model_reasoning_effort =
          parse_string(raw_session.value("model_reasoning_effort", nlohmann::json("")), "");
    }
    session.model_reasoning_effort = policy::NormalizeReasoningEffortValue(session.model_reasoning_effort);
    session.model = parse_string(raw_session.value("model", nlohmann::json("")), "");
    session.title_initialized =
        parse_bool(raw_session.value("titleInitialized", nlohmann::json(nullptr)), false);
    if (!session.title_initialized) {
      session.title_initialized =
          parse_bool(raw_session.value("titleChanged", nlohmann::json(nullptr)), false);
    }
    auto requests_it = raw_session.find("agentStateRequests");
    if (requests_it != raw_session.end() && requests_it->is_object()) {
      session.agent_state_requests = *requests_it;
    }
    auto completed_it = raw_session.find("agentStateCompletedRequests");
    if (completed_it != raw_session.end() && completed_it->is_object()) {
      session.agent_state_completed_requests = *completed_it;
    }
    auto allow_tools_it = raw_session.find("permissionAllowTools");
    if (allow_tools_it != raw_session.end() && allow_tools_it->is_array()) {
      for (const auto& item : *allow_tools_it) {
        if (!item.is_string()) {
          continue;
        }
        const std::string normalized = util::Trim(item.get<std::string>());
        if (!normalized.empty()) {
          session.permission_allow_tools.insert(normalized);
        }
      }
    }
    std::string normalized_permission_mode = util::Trim(session.permission_mode);
    normalized_permission_mode =
        policy::CanonicalizePermissionModeForFlavor(normalized_permission_mode, session.flavor);
    if (normalized_permission_mode.empty()) {
      normalized_permission_mode = policy::DefaultPermissionModeForFlavor(session.flavor);
    }
    session.permission_mode = normalized_permission_mode;
    if (!policy::IsKnownModelMode(session.model_mode) ||
        !policy::IsModelModeAllowedForFlavor(session.model_mode, session.flavor)) {
      session.model_mode = "default";
    }
    if (!policy::IsReasoningEffortAllowedForFlavor(session.model_reasoning_effort, session.flavor)) {
      session.model_reasoning_effort.clear();
    } else if (session.flavor == "codex" && session.model_reasoning_effort.empty()) {
      session.model_reasoning_effort = "medium";
    }
    session.generation = 0;

    const auto messages_it = raw_session.find("messages");
    if (messages_it != raw_session.end() && messages_it->is_array()) {
      int fallback_seq = 1;
      for (const auto& raw_message : *messages_it) {
        if (!raw_message.is_object()) {
          continue;
        }
        MessageRecord message;
        message.id = parse_string(raw_message.value("id", nlohmann::json("")), "");
        if (message.id.empty()) {
          message.id = "msg-" + util::RandomHex(18);
        }
        message.seq =
            std::max(1, static_cast<int>(parse_int64(raw_message.value("seq", nlohmann::json(fallback_seq)),
                                                     fallback_seq)));
        message.local_id = parse_string(raw_message.value("localId", nlohmann::json("")), "");
        message.created_at_ms =
            parse_int64(raw_message.value("createdAt", nlohmann::json(nullptr)), session.updated_at_ms);
        auto content_it = raw_message.find("content");
        if (content_it != raw_message.end()) {
          message.content = *content_it;
        } else {
          message.content = {
              {"role", "agent"},
              {"content", std::string()},
              {"meta", {{"sentFrom", "cli"}}},
          };
        }
        session.messages.push_back(std::move(message));
        ++fallback_seq;
      }
      std::sort(session.messages.begin(), session.messages.end(),
                [](const MessageRecord& lhs, const MessageRecord& rhs) { return lhs.seq < rhs.seq; });
      int seq = 1;
      for (auto& message : session.messages) {
        message.seq = seq++;
      }
      if (!session.messages.empty()) {
        session.updated_at_ms = std::max(session.updated_at_ms, session.messages.back().created_at_ms);
      }
    }

    session_ids_by_ns_[session.ns].push_back(session.id);
    sessions_by_id_.insert_or_assign(session.id, std::move(session));
  }

  if (loaded_from_legacy_json) {
    PersistStateLocked();
  }
}

void CodeAgentManager::PersistStateLocked() {
  if (state_file_path_.empty()) {
    return;
  }

  nlohmann::json root = {
      {"version", kStateSchemaVersion},
      {"savedAt", NowMs()},
      {"machine", {
                      {"id", machine_.id},
                      {"active", machine_.active},
                      {"metadata", machine_.metadata},
                      {"updatedAt", machine_.updated_at_ms},
                  }},
      {"sessions", nlohmann::json::array()},
  };

  std::vector<const SessionRecord*> sessions;
  sessions.reserve(sessions_by_id_.size());
  for (const auto& [_, session] : sessions_by_id_) {
    sessions.push_back(&session);
  }
  std::sort(sessions.begin(), sessions.end(), [](const SessionRecord* lhs, const SessionRecord* rhs) {
    return lhs->created_at_ms < rhs->created_at_ms;
  });

  for (const auto* session : sessions) {
    nlohmann::json serialized = {
        {"id", session->id},
        {"ns", session->ns},
        {"seq", session->seq},
        {"createdAt", session->created_at_ms},
        {"updatedAt", session->updated_at_ms},
        {"active", session->active},
        {"activeAt", session->active_at_ms},
        {"thinkingAt", session->thinking_at_ms},
        {"path", session->path.string()},
        {"host", session->host},
        {"name", session->name},
        {"summaryText", session->summary_text},
        {"summaryUpdatedAt", session->summary_updated_at_ms},
        {"flavor", session->flavor},
        {"machineId", session->machine_id},
        {"permissionMode", session->permission_mode},
        {"modelMode", session->model_mode},
        {"reasoningEffort", session->model_reasoning_effort},
        {"model", session->model},
        {"titleInitialized", session->title_initialized},
        {"agentStateRequests",
         session->agent_state_requests.is_object() ? session->agent_state_requests : nlohmann::json::object()},
        {"agentStateCompletedRequests",
         session->agent_state_completed_requests.is_object() ? session->agent_state_completed_requests
                                                             : nlohmann::json::object()},
        {"permissionAllowTools", nlohmann::json::array()},
        {"messages", nlohmann::json::array()},
    };

    if (!session->permission_allow_tools.empty()) {
      std::vector<std::string> allow_tools(session->permission_allow_tools.begin(),
                                           session->permission_allow_tools.end());
      std::sort(allow_tools.begin(), allow_tools.end());
      for (const auto& tool : allow_tools) {
        serialized["permissionAllowTools"].push_back(tool);
      }
    }

    for (const auto& message : session->messages) {
      serialized["messages"].push_back({
          {"id", message.id},
          {"seq", message.seq},
          {"localId", message.local_id.empty() ? nlohmann::json(nullptr) : nlohmann::json(message.local_id)},
          {"createdAt", message.created_at_ms},
          {"content", message.content},
      });
    }

    root["sessions"].push_back(std::move(serialized));
  }

  std::string serialized_state;
  try {
    serialized_state = root.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  } catch (...) {
    return;
  }
  PersistStatePayloadToSqlite(state_file_path_, serialized_state, NowMs());
}

}  // namespace ferryman::codeagent
