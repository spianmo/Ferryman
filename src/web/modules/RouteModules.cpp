#include "ferryman/web/modules/RouteModules.hpp"

#include "ferryman/web/controllers/HttpController.hpp"
#include "ferryman/web/controllers/WsController.hpp"

namespace ferryman::web::modules {

#if FERRYMAN_WITH_LIBHV

void AuthModule::Register(HttpService* http_service) const {
  if (http_service == nullptr) {
    return;
  }
  auto* controller = &controller_;
  http_service->GET("/api/health", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleHealth(req, resp);
  });
  http_service->POST("/api/auth/login", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleLogin(req, resp);
  });
  http_service->GET("/api/session/me", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleSessionMe(req, resp);
  });
}

void FileTaskModule::Register(HttpService* http_service) const {
  if (http_service == nullptr) {
    return;
  }
  auto* controller = &controller_;
  http_service->GET("/api/files/list", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleFileList(req, resp);
  });
  http_service->GET("/api/files/read", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleFileRead(req, resp);
  });
  http_service->POST("/api/files/write", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleFileWrite(req, resp);
  });
  http_service->POST("/api/tasks/start", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleTaskStart(req, resp);
  });
  http_service->GET("/api/tasks/list", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleTaskList(req, resp);
  });
  http_service->GET("/api/tasks/get", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleTaskGet(req, resp);
  });
  http_service->GET("/api/logs/tail", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleLogsTail(req, resp);
  });
}

void DockurrModule::Register(HttpService* http_service) const {
  if (http_service == nullptr) {
    return;
  }
  auto* controller = &controller_;
  http_service->GET("/api/dockurr/list", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockurrList(req, resp);
  });
  http_service->POST("/api/dockurr/create", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockurrCreate(req, resp);
  });
  http_service->POST("/api/dockurr/start", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockurrStart(req, resp);
  });
  http_service->POST("/api/dockurr/stop", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockurrStop(req, resp);
  });
  http_service->POST("/api/dockurr/restart", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockurrRestart(req, resp);
  });
  http_service->POST("/api/dockurr/delete", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockurrDelete(req, resp);
  });
  http_service->GET("/api/dockurr/logs", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockurrLogs(req, resp);
  });
  http_service->GET("/api/dockurr/inspect", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockurrInspect(req, resp);
  });
}

void DockerModule::Register(HttpService* http_service) const {
  if (http_service == nullptr) {
    return;
  }
  auto* controller = &controller_;
  http_service->GET("/api/docker/list", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockerList(req, resp);
  });
  http_service->POST("/api/docker/service/start", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockerServiceStart(req, resp);
  });
  http_service->POST("/api/docker/start", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockerStart(req, resp);
  });
  http_service->POST("/api/docker/stop", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockerStop(req, resp);
  });
  http_service->POST("/api/docker/restart", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockerRestart(req, resp);
  });
  http_service->GET("/api/docker/logs", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockerLogs(req, resp);
  });
  http_service->GET("/api/docker/inspect", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockerInspect(req, resp);
  });
  http_service->GET("/api/docker/stats", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockerStats(req, resp);
  });
  http_service->GET("/api/docker/processes", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockerProcesses(req, resp);
  });
  http_service->GET("/api/docker/files/list", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockerFileList(req, resp);
  });
  http_service->GET("/api/docker/files/read", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockerFileRead(req, resp);
  });
  http_service->POST("/api/docker/files/write", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleDockerFileWrite(req, resp);
  });
}

void ScreenModule::Register(HttpService* http_service) const {
  if (http_service == nullptr) {
    return;
  }
  auto* controller = &controller_;
  http_service->GET("/api/screen/capabilities", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleScreenCaps(req, resp);
  });
  http_service->GET("/api/screen/sources", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleScreenSources(req, resp);
  });
  http_service->POST("/api/screen/input", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleScreenInput(req, resp);
  });
  http_service->POST("/api/screen/upload/preflight", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleScreenUploadPreflight(req, resp);
  });
  http_service->POST("/api/screen/upload/begin", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleScreenUploadBegin(req, resp);
  });
  http_service->POST("/api/screen/upload/chunk", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleScreenUploadChunk(req, resp);
  });
  http_service->POST("/api/screen/upload/commit", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleScreenUploadCommit(req, resp);
  });
  http_service->POST("/api/screen/upload/cancel", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleScreenUploadCancel(req, resp);
  });
  http_service->POST("/api/codeserver/config", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeServerConfigUpdate(req, resp);
  });
}

void TunnelModule::Register(HttpService* http_service) const {
  if (http_service == nullptr) {
    return;
  }
  auto* controller = &controller_;
  http_service->GET("/api/tunnel/state", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleTunnelState(req, resp);
  });
  http_service->POST("/api/tunnel/config", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleTunnelConfigUpdate(req, resp);
  });
  http_service->POST("/api/tunnel/mapping/upsert", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleTunnelMappingUpsert(req, resp);
  });
  http_service->POST("/api/tunnel/mapping/delete", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleTunnelMappingDelete(req, resp);
  });
  http_service->POST("/api/tunnel/mapping/test", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleTunnelMappingTest(req, resp);
  });
  http_service->GET("/api/tunnel/ports", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleTunnelPorts(req, resp);
  });
}

void CodeAgentModule::Register(HttpService* http_service) const {
  if (http_service == nullptr) {
    return;
  }
  auto* controller = &controller_;
  http_service->GET("/api/codeagent/runner/state", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentRunnerState(req, resp);
  });
  http_service->POST("/api/bind", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentBind(req, resp);
  });
  http_service->GET("/api/events", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentEvents(req, resp);
  });
  http_service->POST("/api/visibility", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentVisibility(req, resp);
  });
  http_service->GET("/api/sessions", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessions(req, resp);
  });
  http_service->PATCH("/api/sessions/{sid}", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionRename(req, resp);
  });
  http_service->Delete("/api/sessions/{sid}", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionDelete(req, resp);
  });
  http_service->GET("/api/sessions/{sid}/messages", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionMessages(req, resp);
  });
  http_service->POST("/api/sessions/{sid}/messages", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionSendMessage(req, resp);
  });
  http_service->POST("/api/sessions/{sid}/resume", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionResume(req, resp);
  });
  http_service->POST("/api/sessions/{sid}/abort", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionAbort(req, resp);
  });
  http_service->POST("/api/sessions/{sid}/archive", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionArchive(req, resp);
  });
  http_service->POST("/api/sessions/{sid}/permission-mode", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionPermissionMode(req, resp);
  });
  http_service->POST("/api/sessions/{sid}/model", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionModelMode(req, resp);
  });
  http_service->POST("/api/sessions/{sid}/reasoning-effort", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionReasoningEffort(req, resp);
  });
  http_service->POST("/api/sessions/{sid}/permissions/{rid}/approve", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionPermissionApprove(req, resp);
  });
  http_service->POST("/api/sessions/{sid}/permissions/{rid}/deny", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionPermissionDeny(req, resp);
  });
  http_service->GET("/api/sessions/{sid}/slash-commands", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionSlashCommands(req, resp);
  });
  http_service->GET("/api/sessions/{sid}/skills", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionSkills(req, resp);
  });
  http_service->GET("/api/sessions/{sid}/git-status", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionGitStatus(req, resp);
  });
  http_service->GET("/api/sessions/{sid}/git-diff-numstat", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionGitDiffNumstat(req, resp);
  });
  http_service->GET("/api/sessions/{sid}/git-diff-file", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionGitDiffFile(req, resp);
  });
  http_service->GET("/api/sessions/{sid}/file", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionFileRead(req, resp);
  });
  http_service->GET("/api/sessions/{sid}/files", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionFileSearch(req, resp);
  });
  http_service->GET("/api/sessions/{sid}/directory", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionDirectory(req, resp);
  });
  http_service->POST("/api/sessions/{sid}/upload", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionUpload(req, resp);
  });
  http_service->POST("/api/sessions/{sid}/upload/delete", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSessionUploadDelete(req, resp);
  });
  http_service->GET("/api/sessions/{sid}", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentSession(req, resp);
  });
  http_service->GET("/api/machines", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentMachines(req, resp);
  });
  http_service->POST("/api/machines/{mid}/spawn", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentMachineSpawn(req, resp);
  });
  http_service->POST("/api/machines/{mid}/paths/exists", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentMachinePathsExists(req, resp);
  });
  http_service->GET("/api/machines/{mid}/directory", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentMachineDirectory(req, resp);
  });
  http_service->GET("/api/push/vapid-public-key", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentPushVapidPublicKey(req, resp);
  });
  http_service->POST("/api/push/subscribe", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentPushSubscribe(req, resp);
  });
  http_service->Delete("/api/push/subscribe", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentPushSubscribe(req, resp);
  });
  http_service->POST("/api/voice/token", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleCodeAgentVoiceToken(req, resp);
  });
}

void AssetModule::Register(HttpService* http_service) const {
  if (http_service == nullptr) {
    return;
  }
  auto* controller = &controller_;
  http_service->Use([controller](HttpRequest* req, HttpResponse* resp) {
    if (req == nullptr || resp == nullptr) {
      return 0;
    }
    if (req->method != HTTP_GET && req->method != HTTP_HEAD) {
      return 0;
    }
    const std::string path = req->path;
    if (path == "/codeagent" || path.rfind("/codeagent/", 0) == 0) {
      return controller->HandleCodeAgentAsset(req, resp);
    }
    return 0;
  });
  http_service->GET("/", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleStaticAsset(req, resp);
  });
  http_service->GET("/index.html", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleStaticAsset(req, resp);
  });
  http_service->GET("/{asset}", [controller](HttpRequest* req, HttpResponse* resp) {
    return controller->HandleStaticAsset(req, resp);
  });
}

void WsModule::Register(hv::WebSocketService* ws_service) const {
  if (ws_service == nullptr) {
    return;
  }
  auto* controller = &controller_;
  ws_service->onopen = [controller](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
    controller->HandleWsOpen(channel, req);
  };
  ws_service->onmessage = [controller](const WebSocketChannelPtr& channel, const std::string& msg) {
    controller->HandleWsMessage(channel, msg);
  };
  ws_service->onclose = [controller](const WebSocketChannelPtr& channel) {
    controller->HandleWsClose(channel);
  };
}

#endif

}  // namespace ferryman::web::modules
