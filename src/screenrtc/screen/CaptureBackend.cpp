#include "ferryman/screenrtc/ScreenService.hpp"

#if defined(__APPLE__)
#include "ferryman/screenrtc/ScreenCaptureKitBridge.hpp"
#include <ApplicationServices/ApplicationServices.h>
#endif

#include <algorithm>
#include <chrono>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace ferryman::web {

namespace {

int64_t EpochMillisNow() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void LogScreenCaptureEvent(const char* level, const std::string& detail) {
  std::cerr << "[ferryman][" << level << "][screen.capture][ts_ms=" << EpochMillisNow() << "] " << detail << '\n';
}

}  // namespace

std::string ScreenService::ActiveCaptureSourceId() const {
  std::lock_guard<std::mutex> lock(capture_source_mu_);
  return active_capture_source_id_;
}

bool ScreenService::StartCapture(int fps, const std::string& source_id, std::string* error) {
  std::lock_guard<std::mutex> lifecycle_lock(capture_lifecycle_mu_);
#if !defined(__APPLE__) && !defined(__linux__) && !defined(_WIN32)
  (void)fps;
  (void)source_id;
  if (error != nullptr) {
    *error = "native capture is unsupported on this platform";
  }
  return false;
#else
  if (!encode_jpeg_.load() && !encode_h264_.load() && !encode_h265_.load() && !encode_vp8_.load() &&
      !encode_vp9_.load() && !encode_av1_.load()) {
    if (error != nullptr) {
      *error = "native capture has no active encoding targets";
    }
    return false;
  }

  std::string source_error;
  const std::string normalized_source_id = NormalizeCaptureSourceId(source_id, &source_error);
  if (normalized_source_id.empty()) {
    if (error != nullptr) {
      *error = source_error.empty() ? "no capture source available" : source_error;
    }
    return false;
  }

  LogScreenCaptureEvent("info", "start requested (requested_source_id=" + source_id +
                                    ", normalized_source_id=" + normalized_source_id +
                                    ", fps=" + std::to_string(fps) +
                                    ", scale_percent=" + std::to_string(capture_scale_percent_.load()) +
                                    ", bitrate_bps=" + std::to_string(video_bitrate_bps_.load()) +
                                    ", encode_targets={jpeg:" + (encode_jpeg_.load() ? "1" : "0") +
                                    ",h264:" + (encode_h264_.load() ? "1" : "0") +
                                    ",h265:" + (encode_h265_.load() ? "1" : "0") +
                                    ",vp8:" + (encode_vp8_.load() ? "1" : "0") +
                                    ",vp9:" + (encode_vp9_.load() ? "1" : "0") +
                                    ",av1:" + (encode_av1_.load() ? "1" : "0") + "})");

  if (capture_thread_.joinable() && !capture_running_.load()) {
    if (capture_thread_.get_id() == std::this_thread::get_id()) {
      LogScreenCaptureEvent("warn", "detaching stale capture thread from same thread context to avoid deadlock");
      capture_thread_.detach();
    } else {
      LogScreenCaptureEvent("info", "joining stale capture thread before restart");
      capture_thread_.join();
    }
  }

  if (capture_running_.exchange(true)) {
    LogScreenCaptureEvent("info", "start ignored: capture already running");
    return true;
  }

  const int safe_fps = std::clamp(fps, 1, 60);
  capture_fps_ = safe_fps;
  if (encode_h264_.load() && !h264_encoder_) {
    h264_encoder_ = CreateVideoEncoder(VideoCodec::kH264);
  }
  if (encode_h265_.load() && !h265_encoder_) {
    h265_encoder_ = CreateVideoEncoder(VideoCodec::kH265);
  }
  if (encode_vp8_.load() && !vp8_encoder_) {
    vp8_encoder_ = CreateVideoEncoder(VideoCodec::kVP8);
  }
  if (encode_vp9_.load() && !vp9_encoder_) {
    vp9_encoder_ = CreateVideoEncoder(VideoCodec::kVP9);
  }
  if (encode_av1_.load() && !av1_encoder_) {
    av1_encoder_ = CreateVideoEncoder(VideoCodec::kAV1);
  }

#if !FERRYMAN_WITH_FFMPEG
  capture_running_ = false;
  if (error != nullptr) {
    *error = "native capture requires ffmpeg (libavcodec/libswscale/libavutil)";
  }
  LogScreenCaptureEvent("error", "start failed: ffmpeg runtime is unavailable");
  return false;
#endif

#if defined(__APPLE__)
  if (!CGPreflightScreenCaptureAccess() && !CGRequestScreenCaptureAccess()) {
    capture_running_ = false;
    if (error != nullptr) {
      *error = "screen capture permission denied by macOS";
    }
    LogScreenCaptureEvent("error", "start failed: macOS screen capture permission denied");
    return false;
  }

  if (!capture_bridge_) {
    capture_bridge_ = std::make_unique<ScreenCaptureKitBridge>();
  }

  if (!capture_bridge_->Start(safe_fps, normalized_source_id, error)) {
    capture_running_ = false;
    LogScreenCaptureEvent("error", "start failed: ScreenCaptureKit bridge start returned false" +
                                       (error != nullptr && !error->empty() ? std::string(", error=") + *error : ""));
    return false;
  }
#endif

  {
    std::lock_guard<std::mutex> lock(capture_source_mu_);
    active_capture_source_id_ = normalized_source_id;
  }

  try {
    capture_thread_ = std::thread([this, safe_fps]() noexcept {
      try {
        CaptureLoop(safe_fps);
      } catch (const std::exception& ex) {
        LogScreenCaptureEvent("error", std::string("native capture thread exception: ") + ex.what());
        capture_running_ = false;
      } catch (...) {
        LogScreenCaptureEvent("error", "native capture thread exception: unknown");
        capture_running_ = false;
      }
    });
    LogScreenCaptureEvent("info", "capture thread started (source_id=" + normalized_source_id +
                                      ", fps=" + std::to_string(safe_fps) + ")");
  } catch (const std::exception& ex) {
    capture_running_ = false;
#if defined(__APPLE__)
    if (capture_bridge_) {
      capture_bridge_->Stop();
    }
#endif
    if (error != nullptr) {
      *error = std::string("failed to start capture thread: ") + ex.what();
    }
    LogScreenCaptureEvent("error", std::string("start failed: unable to spawn capture thread: ") + ex.what());
    return false;
  } catch (...) {
    capture_running_ = false;
#if defined(__APPLE__)
    if (capture_bridge_) {
      capture_bridge_->Stop();
    }
#endif
    if (error != nullptr) {
      *error = "failed to start capture thread";
    }
    LogScreenCaptureEvent("error", "start failed: unable to spawn capture thread (unknown exception)");
    return false;
  }
  LogScreenCaptureEvent("info", "start completed (source_id=" + normalized_source_id +
                                    ", fps=" + std::to_string(safe_fps) + ")");
  return true;
#endif
}

void ScreenService::StopCapture() {
  std::lock_guard<std::mutex> lifecycle_lock(capture_lifecycle_mu_);
  const std::string source_id_before_stop = ActiveCaptureSourceId();
  LogScreenCaptureEvent("info", "stop requested (source_id=" + source_id_before_stop + ")");
  capture_running_ = false;
#if defined(__APPLE__)
  if (capture_bridge_) {
    capture_bridge_->Stop();
  }
#endif
  if (capture_thread_.joinable()) {
    if (capture_thread_.get_id() == std::this_thread::get_id()) {
      LogScreenCaptureEvent("warn", "stop called from capture thread itself; detaching thread");
      capture_thread_.detach();
    } else {
      capture_thread_.join();
    }
  }
  if (h264_encoder_) {
    h264_encoder_->Reset();
  }
  if (h265_encoder_) {
    h265_encoder_->Reset();
  }
  if (vp8_encoder_) {
    vp8_encoder_->Reset();
  }
  if (vp9_encoder_) {
    vp9_encoder_->Reset();
  }
  if (av1_encoder_) {
    av1_encoder_->Reset();
  }
  {
    std::lock_guard<std::mutex> lock(capture_source_mu_);
    active_capture_source_id_.clear();
  }
  LogScreenCaptureEvent("info", "stop completed (source_id=" + source_id_before_stop + ")");
}

std::optional<ScreenService::EncodedFrame> ScreenService::LatestFrame() const {
  std::lock_guard<std::mutex> lock(frame_mu_);
  return latest_frame_;
}

void ScreenService::CaptureLoop(int fps) {
  const auto retry_interval = std::chrono::milliseconds(std::max(1000 / std::max(1, fps), 8));
  uint64_t seq = 0;
  uint64_t failure_count = 0;
  std::string last_failure;

  while (capture_running_) {
    try {
      EncodedFrame frame;
      std::string error;
      if (CaptureFrame(&frame, &error)) {
        if (failure_count > 0) {
          LogScreenCaptureEvent("info", "capture loop recovered after failures (failures=" +
                                            std::to_string(failure_count) +
                                            ", last_error=" +
                                            (last_failure.empty() ? std::string("none") : last_failure) +
                                            ", source_id=" + ActiveCaptureSourceId() + ")");
          failure_count = 0;
          last_failure.clear();
        }
        frame.sequence = ++seq;
        frame.captured_at_ms = EpochMillisNow();
        std::lock_guard<std::mutex> lock(frame_mu_);
        latest_frame_ = std::move(frame);
        continue;
      }
      ++failure_count;
      if (error != last_failure || failure_count == 1 || failure_count % 30 == 0) {
        LogScreenCaptureEvent("warn", "capture frame failed (count=" + std::to_string(failure_count) +
                                        ", source_id=" + ActiveCaptureSourceId() +
                                        ", error=" + (error.empty() ? std::string("unknown") : error) + ")");
        last_failure = error;
      }
      std::this_thread::sleep_for(retry_interval);
    } catch (const std::exception& ex) {
      LogScreenCaptureEvent("error", std::string("native capture loop exception: ") + ex.what());
      capture_running_ = false;
      break;
    } catch (...) {
      LogScreenCaptureEvent("error", "native capture loop exception: unknown");
      capture_running_ = false;
      break;
    }
  }
  LogScreenCaptureEvent("info", "capture loop exited (source_id=" + ActiveCaptureSourceId() +
                                    ", produced_frames=" + std::to_string(seq) + ")");
}

}  // namespace ferryman::web
