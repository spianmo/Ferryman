#pragma once

#include "ferryman/app/AuthApplicationService.hpp"
#include "ferryman/app/CodeAgentApplicationService.hpp"
#include "ferryman/app/DockerApplicationService.hpp"
#include "ferryman/app/DockurrApplicationService.hpp"
#include "ferryman/app/FileApplicationService.hpp"
#include "ferryman/app/ScreenApplicationService.hpp"
#include "ferryman/app/TaskApplicationService.hpp"
#include "ferryman/app/TunnelApplicationService.hpp"
#include "ferryman/core/AuditLogger.hpp"
#include "ferryman/core/ConfigManager.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#if !defined(FERRYMAN_WITH_LIBHV)
#define FERRYMAN_WITH_LIBHV 0
#endif

#if FERRYMAN_WITH_LIBHV
#include "hv/HttpServer.h"
#endif

namespace ferryman::web {

class HttpController {
 public:
  HttpController(core::AppConfig& config, std::atomic<bool>& running, core::AuditLogger& audit_logger,
                 app::AuthApplicationService& auth_application_service,
                 app::FileApplicationService& file_application_service,
                 app::TaskApplicationService& task_application_service,
                 app::DockurrApplicationService& dockurr_application_service,
                 app::DockerApplicationService& docker_application_service,
                 app::ScreenApplicationService& screen_application_service,
                 app::TunnelApplicationService& tunnel_application_service,
                 app::CodeAgentApplicationService& codeagent_application_service)
      : config_(config),
        running_(running),
        audit_logger_(audit_logger),
        auth_application_service_(auth_application_service),
        file_application_service_(file_application_service),
        task_application_service_(task_application_service),
        dockurr_application_service_(dockurr_application_service),
        docker_application_service_(docker_application_service),
        screen_application_service_(screen_application_service),
        tunnel_application_service_(tunnel_application_service),
        codeagent_application_service_(codeagent_application_service) {}

  void CleanupScreenUploads();

#if FERRYMAN_WITH_LIBHV
  int HandleLogin(HttpRequest* req, HttpResponse* resp);
  int HandleSessionMe(HttpRequest* req, HttpResponse* resp);
  int HandleFileList(HttpRequest* req, HttpResponse* resp);
  int HandleFileRead(HttpRequest* req, HttpResponse* resp);
  int HandleFileWrite(HttpRequest* req, HttpResponse* resp);
  int HandleTaskStart(HttpRequest* req, HttpResponse* resp);
  int HandleTaskList(HttpRequest* req, HttpResponse* resp);
  int HandleTaskGet(HttpRequest* req, HttpResponse* resp);
  int HandleLogsTail(HttpRequest* req, HttpResponse* resp);
  int HandleDockurrList(HttpRequest* req, HttpResponse* resp);
  int HandleDockurrCreate(HttpRequest* req, HttpResponse* resp);
  int HandleDockurrStart(HttpRequest* req, HttpResponse* resp);
  int HandleDockurrStop(HttpRequest* req, HttpResponse* resp);
  int HandleDockurrRestart(HttpRequest* req, HttpResponse* resp);
  int HandleDockurrDelete(HttpRequest* req, HttpResponse* resp);
  int HandleDockurrLogs(HttpRequest* req, HttpResponse* resp);
  int HandleDockurrInspect(HttpRequest* req, HttpResponse* resp);
  int HandleDockerList(HttpRequest* req, HttpResponse* resp);
  int HandleDockerServiceStart(HttpRequest* req, HttpResponse* resp);
  int HandleDockerStart(HttpRequest* req, HttpResponse* resp);
  int HandleDockerStop(HttpRequest* req, HttpResponse* resp);
  int HandleDockerRestart(HttpRequest* req, HttpResponse* resp);
  int HandleDockerLogs(HttpRequest* req, HttpResponse* resp);
  int HandleDockerInspect(HttpRequest* req, HttpResponse* resp);
  int HandleDockerStats(HttpRequest* req, HttpResponse* resp);
  int HandleDockerProcesses(HttpRequest* req, HttpResponse* resp);
  int HandleDockerFileList(HttpRequest* req, HttpResponse* resp);
  int HandleDockerFileRead(HttpRequest* req, HttpResponse* resp);
  int HandleDockerFileWrite(HttpRequest* req, HttpResponse* resp);
  int HandleScreenCaps(HttpRequest* req, HttpResponse* resp);
  int HandleScreenSources(HttpRequest* req, HttpResponse* resp);
  int HandleScreenInput(HttpRequest* req, HttpResponse* resp);
  int HandleScreenUploadPreflight(HttpRequest* req, HttpResponse* resp);
  int HandleScreenUploadBegin(HttpRequest* req, HttpResponse* resp);
  int HandleScreenUploadChunk(HttpRequest* req, HttpResponse* resp);
  int HandleScreenUploadCommit(HttpRequest* req, HttpResponse* resp);
  int HandleScreenUploadCancel(HttpRequest* req, HttpResponse* resp);
  int HandleCodeServerConfigUpdate(HttpRequest* req, HttpResponse* resp);
  int HandleTunnelState(HttpRequest* req, HttpResponse* resp);
  int HandleTunnelConfigUpdate(HttpRequest* req, HttpResponse* resp);
  int HandleTunnelMappingUpsert(HttpRequest* req, HttpResponse* resp);
  int HandleTunnelMappingDelete(HttpRequest* req, HttpResponse* resp);
  int HandleTunnelMappingTest(HttpRequest* req, HttpResponse* resp);
  int HandleTunnelPorts(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentRunnerState(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentBind(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentEvents(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentVisibility(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessions(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSession(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionMessages(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionResume(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionSendMessage(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionAbort(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionArchive(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionRename(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionDelete(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionPermissionApprove(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionPermissionDeny(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionPermissionMode(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionModelMode(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionReasoningEffort(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionSlashCommands(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionSkills(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionGitStatus(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionGitDiffNumstat(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionGitDiffFile(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionFileRead(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionFileSearch(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionDirectory(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionUpload(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentSessionUploadDelete(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentMachines(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentMachineSpawn(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentMachinePathsExists(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentMachineDirectory(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentPushVapidPublicKey(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentPushSubscribe(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentVoiceToken(HttpRequest* req, HttpResponse* resp);
  int HandleCodeAgentAsset(HttpRequest* req, HttpResponse* resp);
  int HandleHealth(HttpRequest* req, HttpResponse* resp);
  int HandleStaticAsset(HttpRequest* req, HttpResponse* resp);

  std::string HeaderOf(HttpRequest* req, const std::string& key) const;
  std::string QueryOf(HttpRequest* req, const std::string& key) const;
  std::optional<core::SessionSnapshot> RequireSession(HttpRequest* req, HttpResponse* resp);
  std::optional<std::string> RequireCodeAgentNamespace(HttpRequest* req, HttpResponse* resp,
                                                       std::string* bearer_token = nullptr);
  std::optional<std::filesystem::path> ResolveCodeAgentSessionPath(const std::string& ns,
                                                                   const std::string& session_id,
                                                                   HttpResponse* resp);

  int Json(HttpResponse* resp, int status, const std::string& body) const;
  int Text(HttpResponse* resp, int status, const std::string& body,
           const std::string& content_type = "text/plain; charset=utf-8") const;
#endif

 private:
#if FERRYMAN_WITH_LIBHV
  struct ScreenUploadTransfer {
    std::string owner_session_token;
    std::string transfer_id;
    std::string file_name;
    std::string conflict_strategy = "keep_both";
    std::filesystem::path temp_path;
    std::filesystem::path target_directory;
    std::uint64_t expected_bytes = 0;
    std::uint64_t received_bytes = 0;
    std::ofstream stream;
  };
#endif

  core::AppConfig& config_;
  std::atomic<bool>& running_;
  core::AuditLogger& audit_logger_;
  app::AuthApplicationService& auth_application_service_;
  app::FileApplicationService& file_application_service_;
  app::TaskApplicationService& task_application_service_;
  app::DockurrApplicationService& dockurr_application_service_;
  app::DockerApplicationService& docker_application_service_;
  app::ScreenApplicationService& screen_application_service_;
  app::TunnelApplicationService& tunnel_application_service_;
  app::CodeAgentApplicationService& codeagent_application_service_;
  mutable std::mutex tunnel_mu_;
  mutable std::mutex codeserver_mu_;

#if FERRYMAN_WITH_LIBHV
  std::mutex screen_upload_mu_;
  std::unordered_map<std::string, ScreenUploadTransfer> screen_uploads_;
#endif
};

}  // namespace ferryman::web
