#pragma once

#include "ferryman/codeagent/CodeAgentManager.hpp"
#include "ferryman/core/AuditLogger.hpp"
#include "ferryman/core/SessionManager.hpp"
#include "ferryman/dockurr/DockurrManager.hpp"
#include "ferryman/pty/PtyManager.hpp"
#include "ferryman/tunnel/TunnelManager.hpp"
#include "ferryman/screenrtc/ScreenService.hpp"
#include "ferryman/screenrtc/SystemMonitor.hpp"
#include "ferryman/screenrtc/WebRtcSignalingService.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#if !defined(FERRYMAN_WITH_LIBHV)
#define FERRYMAN_WITH_LIBHV 0
#endif

#if FERRYMAN_WITH_LIBHV
#include "hv/HttpServer.h"
#include "hv/WebSocketServer.h"
#endif

namespace ferryman::web {

class WsController {
 public:
  WsController(std::atomic<bool>& running, core::SessionManager& session_manager, core::AuditLogger& audit_logger,
               codeagent::CodeAgentManager& codeagent_manager, dockurr::DockurrManager& dockurr_manager,
               pty::PtyManager& pty_manager, tunnel::TunnelManager& tunnel_manager,
               ScreenService& screen_service, SystemMonitor& system_monitor,
               WebRtcSignalingService& signaling_service)
      : running_(running),
        session_manager_(session_manager),
        audit_logger_(audit_logger),
        codeagent_manager_(codeagent_manager),
        dockurr_manager_(dockurr_manager),
        pty_manager_(pty_manager),
        tunnel_manager_(tunnel_manager),
        screen_service_(screen_service),
        system_monitor_(system_monitor),
        signaling_service_(signaling_service) {}

  void ResetNativeCaptureState();

#if FERRYMAN_WITH_LIBHV
  void HandleWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req);
  void HandleWsMessage(const WebSocketChannelPtr& channel, const std::string& message);
  void HandleWsClose(const WebSocketChannelPtr& channel);

  void BroadcastTerminalOutput(const std::string& terminal_id, const std::string& chunk);
  void BroadcastLogEntry(const std::string& serialized_entry);
  void BroadcastNativeFrames();
  void BroadcastDockurrSnapshots();
  void BroadcastMonitorSnapshots();
  void BroadcastTunnelSnapshots();

 private:
  struct WsClient {
    std::string channel_type;
    std::string session_token;
    std::string codeagent_namespace;
    std::string codeagent_session_id;
    bool codeagent_events_all = false;
    std::string codeagent_events_session_id;
    std::string codeagent_events_machine_id;
    std::string codeagent_events_visibility = "visible";
    std::string codeagent_events_cursor_key;
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

  void HandleTerminalWsMessage(std::uintptr_t channel_key, const std::string& message);
  void HandleCodeAgentTerminalWsMessage(std::uintptr_t channel_key, const std::string& message);
  void StartCodeAgentEventsWsLoop(std::uintptr_t channel_key);
  void HandleWebRtcWsMessage(std::uintptr_t channel_key, const std::string& message);
  void HandleLogsWsMessage(std::uintptr_t channel_key, const std::string& message);
  void HandleDockurrWsMessage(std::uintptr_t channel_key, const std::string& message);
  void HandleMonitorWsMessage(std::uintptr_t channel_key, const std::string& message);
  void HandleTunnelWsMessage(std::uintptr_t channel_key, const std::string& message);
  void SyncNativeSubscribersToActiveSource();
  std::string HeaderOf(HttpRequest* req, const std::string& key) const;
  void SendToWs(std::uintptr_t channel_key, const std::string& payload);
  NativeCaptureDemand CollectNativeCaptureDemandLocked() const;
  void RefreshNativeCaptureState(const std::string& actor_session_token = "");
#endif

  std::atomic<bool>& running_;
  core::SessionManager& session_manager_;
  core::AuditLogger& audit_logger_;
  codeagent::CodeAgentManager& codeagent_manager_;
  dockurr::DockurrManager& dockurr_manager_;
  pty::PtyManager& pty_manager_;
  tunnel::TunnelManager& tunnel_manager_;
  ScreenService& screen_service_;
  SystemMonitor& system_monitor_;
  WebRtcSignalingService& signaling_service_;

#if FERRYMAN_WITH_LIBHV
  std::mutex ws_mu_;
  std::unordered_map<std::uintptr_t, WsClient> ws_clients_;
  std::atomic<int> active_capture_fps_{0};
  std::atomic<int> active_capture_scale_percent_{75};
  std::atomic<int> active_capture_video_bitrate_bps_{3'000'000};
#endif
};

}  // namespace ferryman::web
