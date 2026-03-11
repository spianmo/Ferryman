#pragma once

#include "ferryman/codeagent/CodeAgentManager.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ferryman::app {

class CodeAgentApplicationService {
 public:
  explicit CodeAgentApplicationService(codeagent::CodeAgentManager& codeagent_manager)
      : codeagent_manager_(codeagent_manager) {}

  nlohmann::json BuildSessionsResponse(const std::string& ns) const {
    return codeagent_manager_.BuildSessionsResponse(ns);
  }

  std::optional<nlohmann::json> BuildSessionResponse(const std::string& ns, const std::string& session_id) const {
    return codeagent_manager_.BuildSessionResponse(ns, session_id);
  }

  std::optional<nlohmann::json> BuildMessagesResponse(const std::string& ns, const std::string& session_id, int limit,
                                                      std::optional<int> before_seq) const {
    return codeagent_manager_.BuildMessagesResponse(ns, session_id, limit, before_seq);
  }

  nlohmann::json BuildMachinesResponse(const std::string& ns) const {
    return codeagent_manager_.BuildMachinesResponse(ns);
  }

  codeagent::SpawnSessionResult SpawnSession(const std::string& ns, const codeagent::SpawnSessionOptions& options) {
    return codeagent_manager_.SpawnSession(ns, options);
  }

  std::optional<std::filesystem::path> ResolveSessionPath(const std::string& ns, const std::string& session_id) const {
    return codeagent_manager_.ResolveSessionPath(ns, session_id);
  }

  nlohmann::json BuildSlashCommandsResponse(const std::string& ns, const std::string& session_id) const {
    return codeagent_manager_.BuildSlashCommandsResponse(ns, session_id);
  }

  nlohmann::json BuildSkillsResponse(const std::string& ns, const std::string& session_id) const {
    return codeagent_manager_.BuildSkillsResponse(ns, session_id);
  }

  bool CheckPathsExist(const std::string& ns, const std::vector<std::string>& paths, nlohmann::json* exists,
                       std::string* error) const {
    return codeagent_manager_.CheckPathsExist(ns, paths, exists, error);
  }

  bool SendSessionMessage(const std::string& ns, const std::string& session_id, const std::string& text,
                          const std::string& local_id, const nlohmann::json& attachments, std::string* error) {
    return codeagent_manager_.SendSessionMessage(ns, session_id, text, local_id, attachments, error);
  }

  bool ResumeSession(const std::string& ns, const std::string& session_id, std::string* error) {
    return codeagent_manager_.ResumeSession(ns, session_id, error);
  }

  bool AbortSession(const std::string& ns, const std::string& session_id, std::string* error) {
    return codeagent_manager_.AbortSession(ns, session_id, error);
  }

  bool ArchiveSession(const std::string& ns, const std::string& session_id, std::string* error) {
    return codeagent_manager_.ArchiveSession(ns, session_id, error);
  }

  bool RenameSession(const std::string& ns, const std::string& session_id, const std::string& name,
                     std::string* error) {
    return codeagent_manager_.RenameSession(ns, session_id, name, error);
  }

  bool DeleteSession(const std::string& ns, const std::string& session_id, std::string* error) {
    return codeagent_manager_.DeleteSession(ns, session_id, error);
  }

  bool SetPermissionMode(const std::string& ns, const std::string& session_id, const std::string& mode,
                         std::string* error) {
    return codeagent_manager_.SetPermissionMode(ns, session_id, mode, error);
  }

  bool ApprovePermissionRequest(const std::string& ns, const std::string& session_id, const std::string& request_id,
                                const std::string& mode, const std::vector<std::string>& allow_tools,
                                const std::string& decision, const nlohmann::json& answers, std::string* error) {
    return codeagent_manager_.ApprovePermissionRequest(ns, session_id, request_id, mode, allow_tools, decision,
                                                       answers, error);
  }

  bool DenyPermissionRequest(const std::string& ns, const std::string& session_id, const std::string& request_id,
                             const std::string& decision, std::string* error) {
    return codeagent_manager_.DenyPermissionRequest(ns, session_id, request_id, decision, error);
  }

  bool SetModelMode(const std::string& ns, const std::string& session_id, const std::string& model,
                    std::string* error) {
    return codeagent_manager_.SetModelMode(ns, session_id, model, error);
  }

  bool SetModelReasoningEffort(const std::string& ns, const std::string& session_id, const std::string& effort,
                               std::string* error) {
    return codeagent_manager_.SetModelReasoningEffort(ns, session_id, effort, error);
  }

  bool SetCodexFast(const std::string& ns, const std::string& session_id, bool enabled, std::string* error) {
    return codeagent_manager_.SetCodexFast(ns, session_id, enabled, error);
  }

  nlohmann::json BuildRunnerState(const std::string& ns) const {
    return codeagent_manager_.BuildRunnerState(ns);
  }

  nlohmann::json DrainEventsForToken(const std::string& bearer_token, const std::string& ns,
                                     const codeagent::EventQuery& query, const std::string& visibility,
                                     int wait_ms = 0) {
    return codeagent_manager_.DrainEventsForToken(bearer_token, ns, query, visibility, wait_ms);
  }

 private:
  codeagent::CodeAgentManager& codeagent_manager_;
};

}  // namespace ferryman::app
