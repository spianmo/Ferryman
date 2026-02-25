#pragma once

#include "ferryman/web/VideoEncoder.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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
    std::string h265_bytes;
    bool h265_keyframe = false;
    std::string vp8_bytes;
    bool vp8_keyframe = false;
    std::string vp9_bytes;
    bool vp9_keyframe = false;
    std::string av1_bytes;
    bool av1_keyframe = false;
  };

  struct CaptureSource {
    std::string id;
    std::string name;
    int width = 0;
    int height = 0;
    bool is_default = false;
  };

  ScreenService();
  ~ScreenService();

  std::string CapabilitiesJson() const;
  std::vector<CaptureSource> ListCaptureSources(std::string* error);
  std::string NormalizeCaptureSourceId(const std::string& requested_source_id, std::string* error);
  std::string ActiveCaptureSourceId() const;
  bool SetRemoteControlEnabled(const std::string& session_token, bool enabled);
  bool CanInjectInput(const std::string& session_token) const;
  bool InjectInputEvent(const std::string& session_token, const InputEvent& event, std::string* error);

  bool StartCapture(int fps, const std::string& source_id, std::string* error);
  void StopCapture();
  std::optional<EncodedFrame> LatestFrame() const;
  bool IsCapturing() const { return capture_running_.load(); }
  void SetEncodingTargets(bool enable_jpeg, bool enable_h264, bool enable_h265, bool enable_vp8,
                          bool enable_vp9, bool enable_av1);
  void SetEncodingProfile(int scale_percent, int video_bitrate_bps);
  bool SupportsH264() const;
  bool SupportsH265() const;
  bool SupportsVP8() const;
  bool SupportsVP9() const;
  bool SupportsAV1() const;

 private:
  void CaptureLoop(int fps);
  bool CaptureFrame(EncodedFrame* frame, std::string* error);
  bool InjectInputEventNative(const InputEvent& event, std::string* error);

  mutable std::mutex mu_;
  std::unordered_map<std::string, bool> control_enabled_;

  mutable std::mutex frame_mu_;
  std::optional<EncodedFrame> latest_frame_;

  std::atomic<bool> capture_running_{false};
  std::mutex capture_lifecycle_mu_;
  std::thread capture_thread_;
  int capture_fps_ = 10;
  std::atomic<bool> encode_jpeg_{false};
  std::atomic<bool> encode_h264_{false};
  std::atomic<bool> encode_h265_{false};
  std::atomic<bool> encode_vp8_{false};
  std::atomic<bool> encode_vp9_{false};
  std::atomic<bool> encode_av1_{false};
  std::atomic<int> capture_scale_percent_{75};
  std::atomic<int> video_bitrate_bps_{3'000'000};
  mutable std::mutex capture_source_mu_;
  std::string active_capture_source_id_;

#if defined(__APPLE__)
  std::unique_ptr<ScreenCaptureKitBridge> capture_bridge_;
#endif

  std::unique_ptr<VideoEncoder> h264_encoder_;
  std::unique_ptr<VideoEncoder> h265_encoder_;
  std::unique_ptr<VideoEncoder> vp8_encoder_;
  std::unique_ptr<VideoEncoder> vp9_encoder_;
  std::unique_ptr<VideoEncoder> av1_encoder_;

  std::mutex pointer_mu_;
  double last_pointer_x_ = 0.0;
  double last_pointer_y_ = 0.0;
};

}  // namespace ferryman::web
