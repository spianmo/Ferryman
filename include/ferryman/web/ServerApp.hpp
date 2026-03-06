#pragma once

#include "ferryman/app/AuthApplicationService.hpp"
#include "ferryman/app/CodeAgentApplicationService.hpp"
#include "ferryman/app/DockerApplicationService.hpp"
#include "ferryman/app/DockurrApplicationService.hpp"
#include "ferryman/app/FileApplicationService.hpp"
#include "ferryman/app/ScreenApplicationService.hpp"
#include "ferryman/app/TaskApplicationService.hpp"
#include "ferryman/app/TunnelApplicationService.hpp"
#include "ferryman/codeagent/CodeAgentManager.hpp"
#include "ferryman/core/AuditLogger.hpp"
#include "ferryman/core/ConfigManager.hpp"
#include "ferryman/core/FileService.hpp"
#include "ferryman/core/SessionManager.hpp"
#include "ferryman/docker/DockerManager.hpp"
#include "ferryman/dockurr/DockurrManager.hpp"
#include "ferryman/pty/PtyManager.hpp"
#include "ferryman/task/TaskManager.hpp"
#include "ferryman/tunnel/TunnelManager.hpp"
#include "ferryman/screenrtc/ScreenService.hpp"
#include "ferryman/screenrtc/SystemMonitor.hpp"
#include "ferryman/screenrtc/WebRtcSignalingService.hpp"
#include "ferryman/web/controllers/HttpController.hpp"
#include "ferryman/web/controllers/WsController.hpp"

#include <atomic>
#include <string>
#include <thread>

#if !defined(FERRYMAN_WITH_LIBHV)
#define FERRYMAN_WITH_LIBHV 0
#endif

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#if FERRYMAN_WITH_LIBHV
#include "hv/HttpServer.h"
#include "hv/WebSocketServer.h"
#endif

#if defined(_WIN32)
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

namespace ferryman::web {

class ServerApp {
 public:
  explicit ServerApp(core::AppConfig config);
  ~ServerApp();

  bool Start();
  void Stop();

 private:
  bool RegisterHttpRoutes();
  bool RegisterWsHandlers();

  core::AppConfig config_;
  core::SessionManager session_manager_;
  core::AuditLogger audit_logger_;
  core::FileService file_service_;
  codeagent::CodeAgentManager codeagent_manager_;
  docker_runtime::DockerManager docker_manager_;
  dockurr::DockurrManager dockurr_manager_;
  task::TaskManager task_manager_;
  pty::PtyManager pty_manager_;
  tunnel::TunnelManager tunnel_manager_;
  ScreenService screen_service_;
  SystemMonitor system_monitor_;
  WebRtcSignalingService signaling_service_;
  app::AuthApplicationService auth_application_service_;
  app::FileApplicationService file_application_service_;
  app::TaskApplicationService task_application_service_;
  app::DockurrApplicationService dockurr_application_service_;
  app::DockerApplicationService docker_application_service_;
  app::ScreenApplicationService screen_application_service_;
  app::TunnelApplicationService tunnel_application_service_;
  app::CodeAgentApplicationService codeagent_application_service_;
  std::atomic<bool> running_{false};
  HttpController http_controller_;
  WsController ws_controller_;

#if FERRYMAN_WITH_LIBHV
  HttpService http_service_;
  http_server_t http_server_{};
  hv::WebSocketService ws_service_;
  hssl_ctx_opt_t tls_ctx_opt_{};
  std::string tls_cert_file_storage_;
  std::string tls_key_file_storage_;

  std::thread http_thread_;
  std::thread native_screen_thread_;
  std::thread dockurr_thread_;
  std::thread monitor_thread_;
#endif
};

}  // namespace ferryman::web
