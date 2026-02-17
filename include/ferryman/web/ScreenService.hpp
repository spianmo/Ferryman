#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace ferryman::web {

class ScreenCaptureKitBridge;

class ScreenService {
 public:
  struct InputEvent {
    std::string type;
    std::string payload;
  };

  struct EncodedFrame {
    uint64_t sequence = 0;
    int width = 0;
    int height = 0;
    int64_t captured_at_ms = 0;
    std::string jpeg_base64;
  };

  ScreenService();
  ~ScreenService();

  std::string CapabilitiesJson() const;
  bool SetRemoteControlEnabled(const std::string& session_token, bool enabled);
  bool CanInjectInput(const std::string& session_token) const;
  bool InjectInputEvent(const std::string& session_token, const InputEvent& event, std::string* error);

  bool StartCapture(int fps, std::string* error);
  void StopCapture();
  std::optional<EncodedFrame> LatestFrame() const;
  bool IsCapturing() const { return capture_running_.load(); }

 private:
  void CaptureLoop(int fps);
  bool CaptureFrame(EncodedFrame* frame, std::string* error);
  bool InjectInputEventNative(const InputEvent& event, std::string* error);

  mutable std::mutex mu_;
  std::unordered_map<std::string, bool> control_enabled_;

  mutable std::mutex frame_mu_;
  std::optional<EncodedFrame> latest_frame_;

  std::atomic<bool> capture_running_{false};
  std::thread capture_thread_;

#if defined(__APPLE__)
  std::unique_ptr<ScreenCaptureKitBridge> capture_bridge_;
#endif

  std::mutex pointer_mu_;
  double last_pointer_x_ = 0.0;
  double last_pointer_y_ = 0.0;
};

}  // namespace ferryman::web
