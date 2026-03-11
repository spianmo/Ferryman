#pragma once

#include "ferryman/core/ConfigManager.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace ferryman::codeagent {

struct SpawnSessionOptions {
  std::string directory;
  std::string agent = "claude";
  std::string model;
  std::string permission_mode;
};

struct SpawnSessionResult {
  std::string type = "error";
  std::string session_id;
  std::string message;
};

struct EventQuery {
  bool all = false;
  std::string session_id;
  std::string machine_id;
};

class CodeAgentManager {
 public:
 explicit CodeAgentManager(const core::AppConfig& config);
  ~CodeAgentManager();

  nlohmann::json BuildSessionsResponse(const std::string& ns, bool include_external = true) const;
  std::optional<nlohmann::json> BuildSessionResponse(const std::string& ns, const std::string& session_id) const;
  std::optional<nlohmann::json> BuildMessagesResponse(const std::string& ns, const std::string& session_id,
                                                      int limit, std::optional<int> before_seq) const;

  nlohmann::json BuildMachinesResponse(const std::string& ns) const;
  SpawnSessionResult SpawnSession(const std::string& ns, const SpawnSessionOptions& options);
  std::optional<std::filesystem::path> ResolveSessionPath(const std::string& ns, const std::string& session_id) const;
  std::optional<std::string> ResolveSessionFlavor(const std::string& ns, const std::string& session_id) const;
  nlohmann::json BuildSlashCommandsResponse(const std::string& ns, const std::string& session_id) const;
  nlohmann::json BuildSkillsResponse(const std::string& ns, const std::string& session_id) const;
  bool CheckPathsExist(const std::string& ns, const std::vector<std::string>& paths,
                       nlohmann::json* exists, std::string* error) const;

  bool SendSessionMessage(const std::string& ns, const std::string& session_id, const std::string& text,
                          const std::string& local_id, const nlohmann::json& attachments, std::string* error);
  bool ResumeSession(const std::string& ns, const std::string& session_id, std::string* error);
  bool AbortSession(const std::string& ns, const std::string& session_id, std::string* error);
  bool ArchiveSession(const std::string& ns, const std::string& session_id, std::string* error);
  bool RenameSession(const std::string& ns, const std::string& session_id, const std::string& name, std::string* error);
  bool DeleteSession(const std::string& ns, const std::string& session_id, std::string* error);
  bool SetPermissionMode(const std::string& ns, const std::string& session_id, const std::string& mode,
                         std::string* error);
  bool ApprovePermissionRequest(const std::string& ns, const std::string& session_id,
                                const std::string& request_id, const std::string& mode,
                                const std::vector<std::string>& allow_tools, const std::string& decision,
                                const nlohmann::json& answers, std::string* error);
  bool DenyPermissionRequest(const std::string& ns, const std::string& session_id, const std::string& request_id,
                             const std::string& decision, std::string* error);
  bool SetModelMode(const std::string& ns, const std::string& session_id, const std::string& model,
                    std::string* error);
  bool SetModelReasoningEffort(const std::string& ns, const std::string& session_id, const std::string& effort,
                               std::string* error);
  bool SetCodexFast(const std::string& ns, const std::string& session_id, bool enabled, std::string* error);

  nlohmann::json BuildRunnerState(const std::string& ns) const;

  nlohmann::json DrainEventsForToken(const std::string& bearer_token, const std::string& ns,
                                     const EventQuery& query, const std::string& visibility,
                                     int wait_ms = 0);

 private:
  struct MessageRecord {
    std::string id;
    int seq = 0;
    std::string local_id;
    std::int64_t created_at_ms = 0;
    nlohmann::json content;
  };

  struct SessionRecord {
    std::string id;
    std::string ns = "default";
    int seq = 1;
    std::int64_t created_at_ms = 0;
    std::int64_t updated_at_ms = 0;
    bool active = true;
    std::int64_t active_at_ms = 0;
    bool thinking = false;
    std::int64_t thinking_at_ms = 0;
    std::filesystem::path path;
    std::filesystem::path transcript_path;
    std::string host;
    std::string name;
    std::string summary_text;
    std::int64_t summary_updated_at_ms = 0;
    std::string flavor = "claude";
    std::string source_session_id;
    std::string machine_id;
    std::string permission_mode = "default";
    std::string model_mode = "default";
    std::string model_reasoning_effort;
    bool codex_fast = false;
    std::string model;
    bool title_initialized = false;
    nlohmann::json agent_state_requests = nlohmann::json::object();
    nlohmann::json agent_state_completed_requests = nlohmann::json::object();
    std::unordered_set<std::string> permission_allow_tools;
    std::unordered_set<std::string> suppressed_permission_call_ids;
    std::uint64_t generation = 0;
    std::vector<MessageRecord> messages;
  };

  struct EventRecord {
    std::uint64_t id = 0;
    std::string ns;
    std::string session_id;
    std::string machine_id;
    nlohmann::json payload;
  };

  struct MachineRecord {
    std::string id;
    bool active = true;
    nlohmann::json metadata;
    std::int64_t updated_at_ms = 0;
  };

  static std::int64_t NowMs();
  static std::string NormalizeAgent(std::string agent);
  static std::string Trim(std::string value);
  static std::string MakeExternalSessionId(std::string_view flavor, std::string_view source_session_id);
  static bool ParseExternalSessionId(std::string_view session_id, std::string* flavor,
                                     std::string* source_session_id);

  std::optional<SessionRecord*> GetMutableSessionLocked(const std::string& ns, const std::string& session_id);
  std::optional<const SessionRecord*> GetSessionLocked(const std::string& ns, const std::string& session_id) const;
  std::vector<SessionRecord> DiscoverExternalSessions(const std::string& ns) const;
  std::optional<SessionRecord> FindExternalSession(const std::string& ns, const std::string& session_id) const;
  bool PopulateExternalSessionMessages(SessionRecord* session, std::string* error) const;
  bool ImportExternalSessionLocked(const std::string& ns, SessionRecord session, std::string* error);

  nlohmann::json BuildSessionJsonLocked(const SessionRecord& session) const;
  nlohmann::json BuildSessionSummaryJsonLocked(const SessionRecord& session) const;
  nlohmann::json BuildMessageJson(const MessageRecord& message) const;
  void ApplySessionRuntimeConfigLocked(SessionRecord& session,
                                       const std::optional<std::string>& permission_mode = std::nullopt,
                                       const std::optional<std::string>& model_mode = std::nullopt,
                                       const std::optional<std::string>& reasoning_effort = std::nullopt,
                                       const std::optional<bool>& codex_fast = std::nullopt);
  MessageRecord UpsertMessageLocked(SessionRecord& session, nlohmann::json content, const std::string& local_id,
                                    bool dedupe_by_local_id, bool* inserted_new);
  void EmitAgentEventMessageLocked(SessionRecord& session, nlohmann::json event_data);
  void PublishMessageReceivedLocked(const SessionRecord& session, const MessageRecord& message);

  void PushEventLocked(const std::string& ns, const nlohmann::json& payload,
                       const std::string& session_id = "", const std::string& machine_id = "");

  struct SessionRunnerState;
  std::shared_ptr<SessionRunnerState> EnsureSessionRunnerLocked(SessionRecord& session, std::string* error);
  void StopSessionRunner(const std::shared_ptr<SessionRunnerState>& runner);
  void StopSessionRunnerLocked(const std::string& session_id);
  bool InterruptSessionRunnerLocked(const std::string& session_id, std::string* error);
  bool StartSessionTurn(const std::string& session_id, std::uint64_t generation, const std::string& prompt,
                        const nlohmann::json& attachments, const std::string& continuation_context,
                        std::string* error);
  bool ResolveSessionRunnerPermissionLocked(SessionRecord& session, const std::string& request_id, bool approved,
                                            const std::string& mode, const std::vector<std::string>& allow_tools,
                                            const std::string& decision, const nlohmann::json& answers,
                                            std::string* error, bool* handled);
  bool AutoResolveSessionRunnerPermissionsLocked(SessionRecord& session, const std::string& normalized_mode,
                                                 std::vector<std::string>* resolved_lines);

  void StartAgentRun(const std::string& session_id, std::uint64_t generation, std::string prompt,
                     std::string continuation_context = {});
  std::string ExecuteAgentCommand(const SessionRecord& session, const std::string& prompt,
                                  const std::string& continuation_context, int* exit_code,
                                  const std::function<void(std::string_view)>& on_chunk) const;
  std::string BuildConversationPrompt(const SessionRecord& session, const std::string& prompt,
                                      const std::string& continuation_context) const;
  std::string BuildAgentCommand(const SessionRecord& session, const std::string& prompt,
                                const std::string& continuation_context) const;
  bool EmitCodexBodyMessage(const std::string& session_id, std::uint64_t generation, const nlohmann::json& body,
                            std::string* summary_text);
  bool AppendAssistantStreamChunk(const std::string& session_id, std::uint64_t generation,
                                  const std::string& message_id, std::string_view chunk);
  void ContinueSessionWithInternalContext(const std::string& session_id, std::string continuation_context);
  void RestoreStateLocked();
  void PersistStateLocked();

  static std::string ShellEscape(const std::string& value);
  static std::string ReadCommandOutput(const std::string& command, int* exit_code,
                                       const std::function<void(std::string_view)>& on_chunk);

  mutable std::mutex mu_;
  mutable std::condition_variable event_cv_;

  std::unordered_map<std::string, SessionRecord> sessions_by_id_;
  std::unordered_map<std::string, std::vector<std::string>> session_ids_by_ns_;

  std::deque<EventRecord> events_;
  std::uint64_t next_event_id_ = 1;
  std::unordered_map<std::string, std::uint64_t> token_event_cursor_;

  MachineRecord machine_;

  std::string claude_cmd_template_;
  std::string codex_cmd_template_;
  std::string cursor_cmd_template_;
  std::string gemini_cmd_template_;
  std::string opencode_cmd_template_;
  std::filesystem::path state_file_path_;
  std::filesystem::path legacy_state_file_path_;
  std::unordered_map<std::string, std::shared_ptr<SessionRunnerState>> session_runners_;
};

}  // namespace ferryman::codeagent
