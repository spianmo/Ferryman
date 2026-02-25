#pragma once

#include "ferryman/core/AuditLogger.hpp"
#include "ferryman/core/ConfigManager.hpp"
#include "ferryman/core/FileService.hpp"
#include "ferryman/core/SessionManager.hpp"
#include "ferryman/docker/DockerManager.hpp"
#include "ferryman/dockurr/DockurrManager.hpp"
#include "ferryman/pty/PtyManager.hpp"
#include "ferryman/task/TaskManager.hpp"
#include "ferryman/web/ScreenService.hpp"
#include "ferryman/web/SystemMonitor.hpp"
#include "ferryman/web/WebRtcSignalingService.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

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
#if FERRYMAN_WITH_LIBHV
  struct WsClient {
    std::string channel_type;
    std::string session_token;
    std::string terminal_id;
    std::string room_id;
    std::string peer_id;
    bool native_stream_subscribed = false;
    std::string native_stream_codec = "jpeg";
    std::string native_stream_source_id;
    int native_stream_fps = 8;
    int native_stream_scale_percent = 100;
    int native_stream_video_bitrate_bps = 3'000'000;
    WebSocketChannelPtr channel;
  };

  struct NativeCaptureDemand {
    size_t subscriber_count = 0;
    bool want_jpeg = false;
    bool want_h264 = false;
    bool want_h265 = false;
    bool want_vp8 = false;
    bool want_vp9 = false;
    bool want_av1 = false;
    int fps = 0;
    int scale_percent = 0;
    int video_bitrate_bps = 0;
    std::string source_id;
  };

  struct ScreenUploadTransfer {
    std::string owner_session_token;
    std::string transfer_id;
    std::string file_name;
    std::filesystem::path temp_path;
    std::filesystem::path target_directory;
    std::uint64_t expected_bytes = 0;
    std::uint64_t received_bytes = 0;
    std::ofstream stream;
  };
#endif

  bool RegisterHttpRoutes();
  bool RegisterWsHandlers();

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
  int HandleDockurrLogs(HttpRequest* req, HttpResponse* resp);
  int HandleDockurrInspect(HttpRequest* req, HttpResponse* resp);
  int HandleDockerList(HttpRequest* req, HttpResponse* resp);
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
  int HandleScreenUploadBegin(HttpRequest* req, HttpResponse* resp);
  int HandleScreenUploadChunk(HttpRequest* req, HttpResponse* resp);
  int HandleScreenUploadCommit(HttpRequest* req, HttpResponse* resp);
  int HandleScreenUploadCancel(HttpRequest* req, HttpResponse* resp);
  int HandleHealth(HttpRequest* req, HttpResponse* resp);
  int HandleStaticAsset(HttpRequest* req, HttpResponse* resp);

  std::string HeaderOf(HttpRequest* req, const std::string& key) const;
  std::string QueryOf(HttpRequest* req, const std::string& key) const;
  std::optional<core::SessionSnapshot> RequireSession(HttpRequest* req, HttpResponse* resp);

  int Json(HttpResponse* resp, int status, const std::string& body) const;
  int Text(HttpResponse* resp, int status, const std::string& body,
           const std::string& content_type = "text/plain; charset=utf-8") const;

  void HandleWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req);
  void HandleWsMessage(const WebSocketChannelPtr& channel, const std::string& message);
  void HandleWsClose(const WebSocketChannelPtr& channel);

  void HandleTerminalWsMessage(std::uintptr_t channel_key, const std::string& message);
  void HandleWebRtcWsMessage(std::uintptr_t channel_key, const std::string& message);
  void HandleLogsWsMessage(std::uintptr_t channel_key, const std::string& message);
  void HandleDockurrWsMessage(std::uintptr_t channel_key, const std::string& message);
  void HandleMonitorWsMessage(std::uintptr_t channel_key, const std::string& message);
  void BroadcastTerminalOutput(const std::string& terminal_id, const std::string& chunk);
  void BroadcastLogEntry(const std::string& serialized_entry);
  void BroadcastNativeFrames();
  void BroadcastDockurrSnapshots();
  void BroadcastMonitorSnapshots();
  void SyncNativeSubscribersToActiveSource();
  void SendToWs(std::uintptr_t channel_key, const std::string& payload);
  NativeCaptureDemand CollectNativeCaptureDemandLocked() const;
  void RefreshNativeCaptureState(const std::string& actor_session_token = "");
#endif

  core::AppConfig config_;
  core::SessionManager session_manager_;
  core::AuditLogger audit_logger_;
  core::FileService file_service_;
  docker_runtime::DockerManager docker_manager_;
  dockurr::DockurrManager dockurr_manager_;
  task::TaskManager task_manager_;
  pty::PtyManager pty_manager_;
  ScreenService screen_service_;
  SystemMonitor system_monitor_;
  WebRtcSignalingService signaling_service_;

  std::atomic<bool> running_{false};

#if FERRYMAN_WITH_LIBHV
  HttpService http_service_;
  http_server_t http_server_{};
  hv::WebSocketService ws_service_;

  std::thread http_thread_;
  std::thread native_screen_thread_;
  std::thread dockurr_thread_;
  std::thread monitor_thread_;

  std::mutex ws_mu_;
  std::unordered_map<std::uintptr_t, WsClient> ws_clients_;
  std::mutex screen_upload_mu_;
  std::unordered_map<std::string, ScreenUploadTransfer> screen_uploads_;
  std::atomic<int> active_capture_fps_{0};
  std::atomic<int> active_capture_scale_percent_{75};
  std::atomic<int> active_capture_video_bitrate_bps_{3'000'000};
#endif
};

}  // namespace ferryman::web
