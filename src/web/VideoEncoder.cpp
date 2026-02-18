#include "ferryman/web/VideoEncoder.hpp"

#include <algorithm>
#include <string>

#if !defined(FERRYMAN_WITH_FFMPEG)
#define FERRYMAN_WITH_FFMPEG 0
#endif

#if FERRYMAN_WITH_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace ferryman::web {

namespace {

const char* VideoCodecToken(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::kH264:
      return "h264";
    case VideoCodec::kH265:
      return "h265";
    case VideoCodec::kVP8:
      return "vp8";
    case VideoCodec::kVP9:
      return "vp9";
  }
  return "unknown";
}

const char* VideoCodecDisplayName(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::kH264:
      return "H.264";
    case VideoCodec::kH265:
      return "H.265";
    case VideoCodec::kVP8:
      return "VP8";
    case VideoCodec::kVP9:
      return "VP9";
  }
  return "unknown";
}

#if FERRYMAN_WITH_FFMPEG

AVCodecID ToAvCodecId(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::kH264:
      return AV_CODEC_ID_H264;
    case VideoCodec::kH265:
      return AV_CODEC_ID_HEVC;
    case VideoCodec::kVP8:
      return AV_CODEC_ID_VP8;
    case VideoCodec::kVP9:
      return AV_CODEC_ID_VP9;
  }
  return AV_CODEC_ID_NONE;
}

const char* VideoToolboxEncoderName(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::kH264:
      return "h264_videotoolbox";
    case VideoCodec::kH265:
      return "hevc_videotoolbox";
    case VideoCodec::kVP8:
    case VideoCodec::kVP9:
      return "";
  }
  return "";
}

std::string AvErrorToString(int code) {
  char error[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(code, error, sizeof(error));
  return std::string(error);
}

bool CodecSupportsPixelFormat(const AVCodec* codec, AVPixelFormat pix_fmt) {
  if (codec == nullptr || codec->pix_fmts == nullptr) {
    return false;
  }
  for (const AVPixelFormat* current = codec->pix_fmts; *current != AV_PIX_FMT_NONE; ++current) {
    if (*current == pix_fmt) {
      return true;
    }
  }
  return false;
}

bool IsVideoToolboxCodec(const AVCodec* codec) {
  return codec != nullptr && codec->name != nullptr &&
         std::string(codec->name).find("videotoolbox") != std::string::npos;
}

const AVCodec* FindEncoder(VideoCodec codec, bool allow_videotoolbox) {
#if defined(__APPLE__)
  if (allow_videotoolbox) {
    const char* hardware_name = VideoToolboxEncoderName(codec);
    if (hardware_name != nullptr && hardware_name[0] != '\0') {
      const AVCodec* hardware = avcodec_find_encoder_by_name(hardware_name);
      if (hardware != nullptr) {
        return hardware;
      }
    }
  }
#endif
  return avcodec_find_encoder(ToAvCodecId(codec));
}

AVPixelFormat PreferredPixelFormat(const AVCodec* codec) {
  if (codec == nullptr) {
    return AV_PIX_FMT_YUV420P;
  }
#if defined(__APPLE__)
  if (IsVideoToolboxCodec(codec) && CodecSupportsPixelFormat(codec, AV_PIX_FMT_NV12)) {
    return AV_PIX_FMT_NV12;
  }
#endif
  if (CodecSupportsPixelFormat(codec, AV_PIX_FMT_YUV420P)) {
    return AV_PIX_FMT_YUV420P;
  }
  if (codec->pix_fmts != nullptr && codec->pix_fmts[0] != AV_PIX_FMT_NONE) {
    return codec->pix_fmts[0];
  }
  return AV_PIX_FMT_YUV420P;
}

class FfmpegVideoEncoder final : public VideoEncoder {
 public:
  explicit FfmpegVideoEncoder(VideoCodec codec_type) : codec_type_(codec_type) {}
  ~FfmpegVideoEncoder() override { Reset(); }

  VideoCodec codec() const override { return codec_type_; }

  void Reset() override {
    if (sws_ctx_ != nullptr) {
      sws_freeContext(sws_ctx_);
      sws_ctx_ = nullptr;
    }
    if (frame_ != nullptr) {
      av_frame_free(&frame_);
      frame_ = nullptr;
    }
    if (codec_ctx_ != nullptr) {
      avcodec_free_context(&codec_ctx_);
      codec_ctx_ = nullptr;
    }
    codec_ = nullptr;
    width_ = 0;
    height_ = 0;
    fps_ = 25;
    bitrate_bps_ = 3'000'000;
    source_pix_fmt_ = AV_PIX_FMT_NONE;
    next_pts_ = 0;
  }

  bool EnsureConfigured(int target_width, int target_height, int target_fps, int target_bitrate_bps,
                        int source_pixel_format, std::string* error) override {
    const AVPixelFormat source_pix_fmt = static_cast<AVPixelFormat>(source_pixel_format);
    if (target_width <= 0 || target_height <= 0 || target_fps <= 0 ||
        target_bitrate_bps <= 0 || source_pix_fmt == AV_PIX_FMT_NONE) {
      if (error != nullptr) {
        *error = std::string("invalid ") + VideoCodecToken(codec_type_) +
                 " encoder dimensions, fps, bitrate, or source format";
      }
      return false;
    }

    if (codec_ctx_ != nullptr &&
        width_ == target_width &&
        height_ == target_height &&
        fps_ == target_fps &&
        bitrate_bps_ == target_bitrate_bps &&
        source_pix_fmt_ == source_pix_fmt) {
      return true;
    }

    Reset();

    codec_ = FindEncoder(codec_type_, allow_videotoolbox_);
    if (codec_ == nullptr) {
      if (error != nullptr) {
        *error = std::string("ffmpeg encoder ") + VideoCodecDisplayName(codec_type_) + " is unavailable";
      }
      return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec_);
    if (codec_ctx_ == nullptr) {
      if (error != nullptr) {
        *error = std::string("failed to allocate ffmpeg ") + VideoCodecToken(codec_type_) +
                 " codec context";
      }
      return false;
    }

    codec_ctx_->width = target_width;
    codec_ctx_->height = target_height;
    codec_ctx_->pix_fmt = PreferredPixelFormat(codec_);
    codec_ctx_->time_base = AVRational{1, target_fps};
    codec_ctx_->framerate = AVRational{target_fps, 1};
    codec_ctx_->gop_size = std::max(target_fps, 10);
    codec_ctx_->max_b_frames = 0;
    codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx_->color_range = AVCOL_RANGE_MPEG;
    codec_ctx_->colorspace = AVCOL_SPC_BT709;
    codec_ctx_->color_primaries = AVCOL_PRI_BT709;
    codec_ctx_->color_trc = AVCOL_TRC_BT709;
    codec_ctx_->bit_rate = static_cast<int64_t>(std::max(target_bitrate_bps, 500'000));

    if (codec_ctx_->priv_data != nullptr) {
      switch (codec_type_) {
        case VideoCodec::kH264:
          (void)av_opt_set(codec_ctx_->priv_data, "profile", "baseline", 0);
          break;
        case VideoCodec::kH265:
          (void)av_opt_set(codec_ctx_->priv_data, "profile", "main", 0);
          break;
        case VideoCodec::kVP8:
        case VideoCodec::kVP9:
          break;
      }

      if (IsVideoToolboxCodec(codec_)) {
        (void)av_opt_set(codec_ctx_->priv_data, "realtime", "1", 0);
        (void)av_opt_set(codec_ctx_->priv_data, "allow_sw", "1", 0);
        (void)av_opt_set(codec_ctx_->priv_data, "prio_speed", "1", 0);
      } else if (codec_type_ == VideoCodec::kH264) {
        (void)av_opt_set(codec_ctx_->priv_data, "preset", "veryfast", 0);
        (void)av_opt_set(codec_ctx_->priv_data, "tune", "zerolatency", 0);
        (void)av_opt_set(codec_ctx_->priv_data, "x264-params",
                         "repeat-headers=1:keyint=30:min-keyint=30:scenecut=0", 0);
      } else if (codec_type_ == VideoCodec::kH265) {
        (void)av_opt_set(codec_ctx_->priv_data, "preset", "ultrafast", 0);
        (void)av_opt_set(codec_ctx_->priv_data, "tune", "zerolatency", 0);
        (void)av_opt_set(codec_ctx_->priv_data, "x265-params",
                         "repeat-headers=1:keyint=30:min-keyint=30:scenecut=0", 0);
      } else {
        (void)av_opt_set(codec_ctx_->priv_data, "deadline", "realtime", 0);
        (void)av_opt_set(codec_ctx_->priv_data, "lag-in-frames", "0", 0);
        (void)av_opt_set(codec_ctx_->priv_data, "error-resilient", "1", 0);
        (void)av_opt_set(codec_ctx_->priv_data, "auto-alt-ref", "0", 0);
        (void)av_opt_set(codec_ctx_->priv_data, "cpu-used",
                         codec_type_ == VideoCodec::kVP9 ? "6" : "8", 0);
      }
    }

    int ret = avcodec_open2(codec_ctx_, codec_, nullptr);
    if (ret < 0) {
      if (error != nullptr) {
        *error = std::string("failed to open ffmpeg ") + VideoCodecToken(codec_type_) +
                 " encoder: " + AvErrorToString(ret);
      }
      Reset();
      return false;
    }

    frame_ = av_frame_alloc();
    if (frame_ == nullptr) {
      if (error != nullptr) {
        *error = std::string("failed to allocate ffmpeg ") + VideoCodecToken(codec_type_) + " frame";
      }
      Reset();
      return false;
    }
    frame_->format = codec_ctx_->pix_fmt;
    frame_->width = target_width;
    frame_->height = target_height;
    frame_->color_range = codec_ctx_->color_range;
    frame_->colorspace = codec_ctx_->colorspace;
    frame_->color_primaries = codec_ctx_->color_primaries;
    frame_->color_trc = codec_ctx_->color_trc;

    ret = av_frame_get_buffer(frame_, 32);
    if (ret < 0) {
      if (error != nullptr) {
        *error = std::string("failed to allocate ffmpeg ") + VideoCodecToken(codec_type_) +
                 " frame buffer: " + AvErrorToString(ret);
      }
      Reset();
      return false;
    }

    sws_ctx_ = sws_getContext(target_width, target_height, source_pix_fmt,
                              target_width, target_height, codec_ctx_->pix_fmt,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (sws_ctx_ == nullptr) {
      if (error != nullptr) {
        *error = std::string("failed to create ffmpeg ") + VideoCodecToken(codec_type_) +
                 " sws context";
      }
      Reset();
      return false;
    }

    width_ = target_width;
    height_ = target_height;
    fps_ = target_fps;
    bitrate_bps_ = target_bitrate_bps;
    source_pix_fmt_ = source_pix_fmt;
    next_pts_ = 0;
    return true;
  }

  bool EncodeFrame(const uint8_t* source, int stride, std::string* encoded_bytes, bool* keyframe,
                   std::string* error) override {
    if (source == nullptr || stride <= 0 || encoded_bytes == nullptr || keyframe == nullptr ||
        codec_ctx_ == nullptr || frame_ == nullptr || sws_ctx_ == nullptr) {
      if (error != nullptr) {
        *error = std::string(VideoCodecToken(codec_type_)) + " encoder is not initialized";
      }
      return false;
    }

    int ret = av_frame_make_writable(frame_);
    if (ret < 0) {
      if (error != nullptr) {
        *error = std::string("failed to make ffmpeg ") + VideoCodecToken(codec_type_) +
                 " frame writable: " + AvErrorToString(ret);
      }
      return false;
    }

    const uint8_t* src_data[1] = {source};
    int src_linesize[1] = {stride};
    sws_scale(sws_ctx_, src_data, src_linesize, 0, height_, frame_->data, frame_->linesize);
    frame_->color_range = codec_ctx_->color_range;
    frame_->colorspace = codec_ctx_->colorspace;
    frame_->color_primaries = codec_ctx_->color_primaries;
    frame_->color_trc = codec_ctx_->color_trc;

    frame_->pts = next_pts_++;
    ret = avcodec_send_frame(codec_ctx_, frame_);
    if (ret < 0) {
      if (TryFallbackToSoftware(error)) {
        return EncodeFrame(source, stride, encoded_bytes, keyframe, error);
      }
      if (error != nullptr) {
        *error = std::string("failed to send frame to ffmpeg ") + VideoCodecToken(codec_type_) +
                 " encoder: " + AvErrorToString(ret);
      }
      return false;
    }

    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) {
      if (error != nullptr) {
        *error = std::string("failed to allocate ffmpeg ") + VideoCodecToken(codec_type_) + " packet";
      }
      return false;
    }

    encoded_bytes->clear();
    *keyframe = false;
    bool got_packet = false;

    while (true) {
      ret = avcodec_receive_packet(codec_ctx_, packet);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        break;
      }
      if (ret < 0) {
        if (TryFallbackToSoftware(error)) {
          av_packet_free(&packet);
          return EncodeFrame(source, stride, encoded_bytes, keyframe, error);
        }
        if (error != nullptr) {
          *error = std::string("failed to receive ffmpeg ") + VideoCodecToken(codec_type_) +
                   " packet: " + AvErrorToString(ret);
        }
        av_packet_free(&packet);
        return false;
      }

      got_packet = true;
      if ((packet->flags & AV_PKT_FLAG_KEY) != 0) {
        *keyframe = true;
      }
      if (packet->size > 0 && packet->data != nullptr) {
        encoded_bytes->append(reinterpret_cast<const char*>(packet->data),
                              static_cast<size_t>(packet->size));
      }
      av_packet_unref(packet);
    }

    av_packet_free(&packet);

    if (!got_packet || encoded_bytes->empty()) {
      if (error != nullptr) {
        *error = std::string("ffmpeg ") + VideoCodecToken(codec_type_) + " encoder produced empty packet";
      }
      return false;
    }
    return true;
  }

 private:
  bool TryFallbackToSoftware(std::string* error) {
    if (!IsVideoToolboxCodec(codec_) || !allow_videotoolbox_) {
      return false;
    }

    const int fallback_width = width_;
    const int fallback_height = height_;
    const int fallback_fps = fps_;
    const int fallback_bitrate_bps = bitrate_bps_;
    const int fallback_source_pix_fmt = static_cast<int>(source_pix_fmt_);
    allow_videotoolbox_ = false;
    Reset();
    return EnsureConfigured(fallback_width, fallback_height, fallback_fps, fallback_bitrate_bps,
                            fallback_source_pix_fmt, error);
  }

  VideoCodec codec_type_;
  bool allow_videotoolbox_ = true;
  const AVCodec* codec_ = nullptr;
  AVCodecContext* codec_ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  SwsContext* sws_ctx_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int fps_ = 25;
  int bitrate_bps_ = 3'000'000;
  AVPixelFormat source_pix_fmt_ = AV_PIX_FMT_NONE;
  int64_t next_pts_ = 0;
};

#endif

class UnsupportedVideoEncoder final : public VideoEncoder {
 public:
  explicit UnsupportedVideoEncoder(VideoCodec codec_type) : codec_type_(codec_type) {}

  VideoCodec codec() const override { return codec_type_; }

  void Reset() override {}

  bool EnsureConfigured(int width, int height, int fps, int bitrate_bps, int source_pixel_format,
                        std::string* error) override {
    (void)width;
    (void)height;
    (void)fps;
    (void)bitrate_bps;
    (void)source_pixel_format;
    if (error != nullptr) {
      *error = std::string(VideoCodecDisplayName(codec_type_)) +
               " encoder requires ffmpeg (libavcodec/libswscale/libavutil)";
    }
    return false;
  }

  bool EncodeFrame(const uint8_t* source, int stride, std::string* encoded_bytes, bool* keyframe,
                   std::string* error) override {
    (void)source;
    (void)stride;
    (void)encoded_bytes;
    (void)keyframe;
    if (error != nullptr) {
      *error = std::string(VideoCodecDisplayName(codec_type_)) +
               " encoder requires ffmpeg (libavcodec/libswscale/libavutil)";
    }
    return false;
  }

 private:
  VideoCodec codec_type_;
};

}  // namespace

const char* VideoCodecName(VideoCodec codec) {
  return VideoCodecToken(codec);
}

bool SupportsVideoCodec(VideoCodec codec) {
#if FERRYMAN_WITH_FFMPEG
  return FindEncoder(codec, true) != nullptr;
#else
  (void)codec;
  return false;
#endif
}

std::unique_ptr<VideoEncoder> CreateVideoEncoder(VideoCodec codec) {
#if FERRYMAN_WITH_FFMPEG
  return std::make_unique<FfmpegVideoEncoder>(codec);
#else
  return std::make_unique<UnsupportedVideoEncoder>(codec);
#endif
}

}  // namespace ferryman::web
