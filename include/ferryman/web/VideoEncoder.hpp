#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace ferryman::web {

enum class VideoCodec {
  kH264,
  kH265,
  kVP8,
  kVP9,
};

const char* VideoCodecName(VideoCodec codec);
bool SupportsVideoCodec(VideoCodec codec);

class VideoEncoder {
 public:
  virtual ~VideoEncoder() = default;

  virtual VideoCodec codec() const = 0;
  virtual void Reset() = 0;
  virtual bool EnsureConfigured(int width, int height, int fps, int bitrate_bps, int source_pixel_format,
                                std::string* error) = 0;
  virtual bool EncodeFrame(const uint8_t* source, int stride, std::string* encoded_bytes, bool* keyframe,
                           std::string* error) = 0;
};

std::unique_ptr<VideoEncoder> CreateVideoEncoder(VideoCodec codec);

}  // namespace ferryman::web
