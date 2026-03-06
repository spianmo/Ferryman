#pragma once

#include "ferryman/screenrtc/ScreenService.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ferryman::app {

class ScreenApplicationService {
 public:
  explicit ScreenApplicationService(web::ScreenService& screen_service) : screen_service_(screen_service) {}

  std::string CapabilitiesJson() const {
    return screen_service_.CapabilitiesJson();
  }

  std::vector<web::ScreenService::CaptureSource> ListCaptureSources(std::string* error) {
    return screen_service_.ListCaptureSources(error);
  }

  std::string NormalizeCaptureSourceId(const std::string& requested_source_id, std::string* error) {
    return screen_service_.NormalizeCaptureSourceId(requested_source_id, error);
  }

  std::string ActiveCaptureSourceId() const {
    return screen_service_.ActiveCaptureSourceId();
  }

  bool SetRemoteControlEnabled(const std::string& session_token, bool enabled) {
    return screen_service_.SetRemoteControlEnabled(session_token, enabled);
  }

  bool CanInjectInput(const std::string& session_token) const {
    return screen_service_.CanInjectInput(session_token);
  }

  bool InjectInputEvent(const std::string& session_token, const web::ScreenService::InputEvent& event,
                        std::string* error) {
    return screen_service_.InjectInputEvent(session_token, event, error);
  }

  bool StartCapture(int fps, const std::string& source_id, std::string* error) {
    return screen_service_.StartCapture(fps, source_id, error);
  }

  void StopCapture() {
    screen_service_.StopCapture();
  }

  std::optional<web::ScreenService::EncodedFrame> LatestFrame() const {
    return screen_service_.LatestFrame();
  }

  bool IsCapturing() const {
    return screen_service_.IsCapturing();
  }

  void SetEncodingTargets(bool enable_jpeg, bool enable_h264, bool enable_h265, bool enable_vp8, bool enable_vp9,
                          bool enable_av1) {
    screen_service_.SetEncodingTargets(enable_jpeg, enable_h264, enable_h265, enable_vp8, enable_vp9, enable_av1);
  }

  void SetEncodingProfile(int scale_percent, int video_bitrate_bps) {
    screen_service_.SetEncodingProfile(scale_percent, video_bitrate_bps);
  }

  bool SupportsH264() const {
    return screen_service_.SupportsH264();
  }

  bool SupportsH265() const {
    return screen_service_.SupportsH265();
  }

  bool SupportsVP8() const {
    return screen_service_.SupportsVP8();
  }

  bool SupportsVP9() const {
    return screen_service_.SupportsVP9();
  }

  bool SupportsAV1() const {
    return screen_service_.SupportsAV1();
  }

 private:
  web::ScreenService& screen_service_;
};

}  // namespace ferryman::app
