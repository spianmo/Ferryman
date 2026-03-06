#include "ferryman/web/ServerApp.hpp"

#include "ferryman/api/ResponseUtil.hpp"
#include "ferryman/tunnel/PortInspector.hpp"
#include "ferryman/util/Random.hpp"
#include "ferryman/util/StringUtil.hpp"
#include "ferryman/util/Time.hpp"
#include "ferryman/web/EmbeddedAssets.hpp"
#include "ferryman/web/modules/RouteModules.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#endif

#if FERRYMAN_WITH_LIBHV
#include "hv/hlog.h"
#endif

namespace ferryman::web {

#include "serverapp/ServerAppSupport.inc"

ServerApp::ServerApp(core::AppConfig config)
    : config_(std::move(config)),
      audit_logger_(config_.audit_log_path),
      file_service_(config_.workspace_root),
      codeagent_manager_(config_),
      docker_manager_(config_.workspace_root),
      dockurr_manager_(config_.workspace_root),
      tunnel_manager_(&audit_logger_),
      auth_application_service_(session_manager_, audit_logger_),
      file_application_service_(file_service_),
      task_application_service_(task_manager_),
      dockurr_application_service_(dockurr_manager_),
      docker_application_service_(docker_manager_),
      screen_application_service_(screen_service_),
      tunnel_application_service_(tunnel_manager_),
      codeagent_application_service_(codeagent_manager_),
      http_controller_(config_, running_, audit_logger_, auth_application_service_, file_application_service_,
                       task_application_service_, dockurr_application_service_, docker_application_service_,
                       screen_application_service_, tunnel_application_service_, codeagent_application_service_),
      ws_controller_(running_, session_manager_, audit_logger_, codeagent_manager_, dockurr_manager_, pty_manager_,
                     tunnel_manager_, screen_service_, system_monitor_, signaling_service_) {
#if FERRYMAN_WITH_LIBHV
  audit_logger_.SetRealtimeCallback([this](const std::string& serialized_entry) {
    ws_controller_.BroadcastLogEntry(serialized_entry);
  });
  tunnel_manager_.SetRuntimeUpdateCallback([this]() {
    ws_controller_.BroadcastTunnelSnapshots();
  });
#endif
}

ServerApp::~ServerApp() {
  Stop();
}

bool ServerApp::Start() {
#if !FERRYMAN_WITH_LIBHV
  std::cerr << "[ferryman] libhv is not enabled. Reconfigure with FERRYMAN_WITH_LIBHV=ON.\n";
  return false;
#else
  if (running_.exchange(true)) {
    return true;
  }

  // WebSocket and HTTP share one listener.
  config_.ws_port = config_.http_port;

  tunnel_manager_.Configure(config_.tunnel_proxy_host, config_.tunnel_proxy_port, config_.tunnel_proxy_token,
                            config_.tunnel_mappings);
  tunnel_manager_.Start();

  // Disable libhv file logger to avoid generating libhv.yyyymmdd.log files.
  hlog_set_handler(stderr_logger);

  if (!RegisterHttpRoutes() || !RegisterWsHandlers()) {
    audit_logger_.AppendSystem("error", "server.start", "failed to register HTTP/WS routes");
    tunnel_manager_.Stop();
    running_ = false;
    return false;
  }
  screen_service_.SetEncodingTargets(false, false, false, false, false, false);

  pty_manager_.SetOutputCallback([this](const std::string& terminal_id, const std::string& chunk) {
    ws_controller_.BroadcastTerminalOutput(terminal_id, chunk);
  });

  http_server_.service = &http_service_;
  http_server_.ws = &ws_service_;
  std::snprintf(http_server_.host, sizeof(http_server_.host), "%s", config_.http_host.c_str());
  http_server_.port = config_.http_port;
  http_server_.https_port = 0;
  const unsigned hw_threads = std::thread::hardware_concurrency();
  const unsigned target_threads = std::clamp(hw_threads > 0 ? hw_threads : 4U, 4U, 16U);
  http_server_.worker_threads = static_cast<int>(target_threads);
  http_server_.ssl_ctx = nullptr;
  http_server_.alloced_ssl_ctx = 0;

  bool https_active = false;
  if (config_.https_enabled) {
    const std::string ssl_backend = hssl_backend();
    audit_logger_.AppendSystem("info", "server.tls", "libhv ssl backend: " + ssl_backend);
    if (!HV_WITH_SSL) {
      audit_logger_.AppendSystem("warn", "server.tls",
                                 "https_enabled=true but libhv was built without SSL/TLS; fallback to http/ws only");
    } else if (ssl_backend == "appletls") {
      // libhv 1.3.3 appletls backend does not load cert/key from hssl_ctx_opt_t for server mode.
      // This causes browser TLS handshakes to fail immediately.
      audit_logger_.AppendSystem(
          "warn", "server.tls",
          "https_enabled=true but libhv ssl backend is appletls; server-side TLS handshake is unstable on this "
          "backend. Rebuild dependencies with libhv[ssl] (OpenSSL) and restart.");
    } else {
      std::filesystem::path tls_crt_file;
      std::filesystem::path tls_key_file;
      std::string tls_error;
      if (!EnsureTlsCertificateFiles(config_, &tls_crt_file, &tls_key_file, &tls_error)) {
        audit_logger_.AppendSystem("warn", "server.tls",
                                   "failed to prepare tls certificate; fallback to http/ws only: " +
                                       (tls_error.empty() ? std::string("unknown error") : tls_error));
      } else {
        const bool should_persist_tls_paths = config_.tls_cert_file.empty() && config_.tls_key_file.empty();
        config_.tls_cert_file = tls_crt_file;
        config_.tls_key_file = tls_key_file;
        if (should_persist_tls_paths) {
          std::string persist_error;
          if (!PersistTlsPathsToConfig(config_, tls_crt_file, tls_key_file, &persist_error)) {
            audit_logger_.AppendSystem("warn", "server.tls",
                                       "failed to persist tls cert/key paths to config: " +
                                           (persist_error.empty() ? std::string("unknown error") : persist_error));
          } else {
            audit_logger_.AppendSystem("info", "server.tls",
                                       "persisted tls cert/key paths to config (crt=" + tls_crt_file.string() +
                                           ", key=" + tls_key_file.string() + ")");
          }
        }

        // libhv backends (e.g. appletls) keep a pointer to hssl_ctx_opt_t instead of deep-copying it.
        // Keep TLS option payload alive for the whole ServerApp lifetime.
        tls_cert_file_storage_ = tls_crt_file.string();
        tls_key_file_storage_ = tls_key_file.string();
        tls_ctx_opt_ = {};
        tls_ctx_opt_.crt_file = tls_cert_file_storage_.c_str();
        tls_ctx_opt_.key_file = tls_key_file_storage_.c_str();
        tls_ctx_opt_.verify_peer = 0;
        tls_ctx_opt_.endpoint = HSSL_SERVER;
        hssl_ctx_t tls_ctx = hssl_ctx_new(&tls_ctx_opt_);
        if (tls_ctx == nullptr) {
          audit_logger_.AppendSystem("warn", "server.tls", "failed to initialize ssl context; fallback to http/ws only");
        } else {
          http_server_.ssl_ctx = tls_ctx;
          http_server_.alloced_ssl_ctx = 1;
          http_server_.https_port = config_.https_port;
          https_active = true;
          audit_logger_.AppendSystem("info", "server.tls",
                                     "enabled tls (crt=" + tls_crt_file.string() + ", key=" + tls_key_file.string() +
                                         ", https_port=" + std::to_string(config_.https_port) +
                                         ", backend=" + ssl_backend + ")");
        }
      }
    }
  }
  config_.https_enabled = https_active;

  http_thread_ = std::thread([this]() {
    http_server_run(&http_server_);
  });

  native_screen_thread_ = std::thread([this]() {
    ws_controller_.BroadcastNativeFrames();
  });
  dockurr_thread_ = std::thread([this]() {
    ws_controller_.BroadcastDockurrSnapshots();
  });
  monitor_thread_ = std::thread([this]() {
    ws_controller_.BroadcastMonitorSnapshots();
  });

  std::cout << "[ferryman] http: http://" << config_.http_host << ':' << config_.http_port << '\n';
  std::cout << "[ferryman] ws:   ws://" << config_.http_host << ':' << config_.ws_port << '\n';
  if (https_active) {
    std::cout << "[ferryman] https: https://" << config_.http_host << ':' << config_.https_port << '\n';
    std::cout << "[ferryman] wss:  wss://" << config_.http_host << ':' << config_.https_port << '\n';
  } else {
    std::cout << "[ferryman] https: disabled\n";
    std::cout << "[ferryman] wss:  disabled\n";
  }

  std::string server_start_message = "http=" + config_.http_host + ":" + std::to_string(config_.http_port) +
                                     ", ws=" + config_.http_host + ":" + std::to_string(config_.ws_port);
  if (https_active) {
    server_start_message += ", https=" + config_.http_host + ":" + std::to_string(config_.https_port) +
                            ", wss=" + config_.http_host + ":" + std::to_string(config_.https_port);
  } else {
    server_start_message += ", https=disabled, wss=disabled";
  }
  audit_logger_.AppendSystem("info", "server.start", server_start_message);

  if (DetectCodeServerInstalled()) {
    const int code_server_port = LoadCodeServerPort(config_);
    std::string startup_detail;
    const bool ensured = EnsureCodeServerRunning(config_, code_server_port, &startup_detail);
    audit_logger_.AppendSystem(ensured ? "info" : "warn", "codeserver.startup", startup_detail);
  }
  return true;
#endif
}

void ServerApp::Stop() {
  bool was_running = false;
#if FERRYMAN_WITH_LIBHV
  was_running = running_.exchange(false);
  if (was_running) {
    http_server_stop(&http_server_);

    if (http_thread_.joinable()) {
      http_thread_.join();
    }
  }

  if (native_screen_thread_.joinable()) {
    native_screen_thread_.join();
  }
  if (dockurr_thread_.joinable()) {
    dockurr_thread_.join();
  }
  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }
#endif
  http_controller_.CleanupScreenUploads();
  screen_service_.StopCapture();
#if FERRYMAN_WITH_LIBHV
  ws_controller_.ResetNativeCaptureState();
#endif
  pty_manager_.Shutdown();
  tunnel_manager_.Stop();
  if (was_running) {
    std::vector<std::string> stopped_names;
    std::string cleanup_error;
    if (!dockurr_manager_.StopTemporaryVms(&stopped_names, &cleanup_error)) {
      if (!cleanup_error.empty()) {
        audit_logger_.AppendSystem("warn", "dockurr.cleanup", cleanup_error);
      }
    } else if (!stopped_names.empty()) {
      audit_logger_.AppendSystem("info", "dockurr.cleanup",
                                 "stopped temporary vm(s): " + JoinStrings(stopped_names, ", "));
    }
  }
  if (was_running) {
    audit_logger_.AppendSystem("info", "server.stop", "runtime shutdown completed");
  }
}

bool ServerApp::RegisterHttpRoutes() {
#if !FERRYMAN_WITH_LIBHV
  return false;
#else
  modules::AuthModule(http_controller_).Register(&http_service_);
  modules::FileTaskModule(http_controller_).Register(&http_service_);
  modules::DockurrModule(http_controller_).Register(&http_service_);
  modules::DockerModule(http_controller_).Register(&http_service_);
  modules::ScreenModule(http_controller_).Register(&http_service_);
  modules::TunnelModule(http_controller_).Register(&http_service_);
  modules::CodeAgentModule(http_controller_).Register(&http_service_);
  modules::AssetModule(http_controller_).Register(&http_service_);
  return true;
#endif
}

bool ServerApp::RegisterWsHandlers() {
#if !FERRYMAN_WITH_LIBHV
  return false;
#else
  modules::WsModule(ws_controller_).Register(&ws_service_);
  return true;
#endif
}

}  // namespace ferryman::web
