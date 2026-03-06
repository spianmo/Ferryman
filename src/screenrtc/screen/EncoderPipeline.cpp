#include "ferryman/screenrtc/ScreenService.hpp"

#include "ferryman/screenrtc/VideoEncoder.hpp"

#include <algorithm>

namespace ferryman::web {

bool ScreenService::SupportsH264() const {
  return SupportsVideoCodec(VideoCodec::kH264);
}

bool ScreenService::SupportsH265() const {
  return SupportsVideoCodec(VideoCodec::kH265);
}

bool ScreenService::SupportsVP8() const {
  return SupportsVideoCodec(VideoCodec::kVP8);
}

bool ScreenService::SupportsVP9() const {
  return SupportsVideoCodec(VideoCodec::kVP9);
}

bool ScreenService::SupportsAV1() const {
  return SupportsVideoCodec(VideoCodec::kAV1);
}

void ScreenService::SetEncodingTargets(bool enable_jpeg, bool enable_h264, bool enable_h265, bool enable_vp8,
                                       bool enable_vp9, bool enable_av1) {
  encode_jpeg_.store(enable_jpeg);
  encode_h264_.store(enable_h264);
  encode_h265_.store(enable_h265);
  encode_vp8_.store(enable_vp8);
  encode_vp9_.store(enable_vp9);
  encode_av1_.store(enable_av1);
}

void ScreenService::SetEncodingProfile(int scale_percent, int video_bitrate_bps) {
  capture_scale_percent_.store(std::clamp(scale_percent, 40, 100));
  video_bitrate_bps_.store(std::clamp(video_bitrate_bps, 500'000, 12'000'000));
}

}  // namespace ferryman::web
