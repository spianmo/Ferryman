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
    std::string jpeg_bytes;
    std::string h264_bytes;
    bool h264_keyframe = false;
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
  void SetEncodingTargets(bool enable_jpeg, bool enable_h264);
  void SetEncodingProfile(int scale_percent, int h264_bitrate_bps);
  bool SupportsH264() const;

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
  int capture_fps_ = 10;
  std::atomic<bool> encode_jpeg_{false};
  std::atomic<bool> encode_h264_{false};
  std::atomic<int> capture_scale_percent_{75};
  std::atomic<int> h264_bitrate_bps_{3'000'000};

#if defined(__APPLE__)
  std::unique_ptr<ScreenCaptureKitBridge> capture_bridge_;
#endif

  struct H264EncoderContext;
  std::unique_ptr<H264EncoderContext> h264_encoder_;

  std::mutex pointer_mu_;
  double last_pointer_x_ = 0.0;
  double last_pointer_y_ = 0.0;
};

}  // namespace ferryman::web
