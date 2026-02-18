#include "ferryman/web/ScreenCaptureKitBridge.hpp"

#if defined(__APPLE__)

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>

namespace ferryman::web {
class ScreenCaptureKitBridgeImpl;
}

static void FerrymanDispatchSampleBuffer(void* owner, CMSampleBufferRef sample_buffer,
                                         SCStreamOutputType type);

@interface FerrymanStreamOutput : NSObject <SCStreamOutput>
- (instancetype)initWithOwner:(void*)owner;
@end

@implementation FerrymanStreamOutput {
  void* owner_;
}

- (instancetype)initWithOwner:(void*)owner {
  self = [super init];
  if (self != nil) {
    owner_ = owner;
  }
  return self;
}

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
  (void)stream;
  FerrymanDispatchSampleBuffer(owner_, sampleBuffer, type);
}

@end

namespace ferryman::web {

namespace {

constexpr int kStartTimeoutMs = 8000;
constexpr int kStopTimeoutMs = 3000;

std::string NsStringToStdString(NSString* value) {
  if (value == nil) {
    return "";
  }
  const char* utf8 = value.UTF8String;
  if (utf8 == nullptr) {
    return "";
  }
  return std::string(utf8);
}

std::string NSErrorToString(NSError* error) {
  if (error == nil) {
    return "unknown error";
  }
  return NsStringToStdString(error.localizedDescription);
}

}  // namespace

class ScreenCaptureKitBridgeImpl {
 public:
  bool Start(int fps, std::string* error);
  void Stop();
  bool WaitForFrame(int timeout_ms, ScreenCaptureKitBridge::RawFrame* frame, std::string* error);
  void OnSampleBuffer(CMSampleBufferRef sample_buffer, SCStreamOutputType type);

 private:
  std::mutex mu_;
  std::condition_variable cv_;

  bool started_ = false;
  uint64_t frame_sequence_ = 0;
  uint64_t consumed_sequence_ = 0;

  int width_ = 0;
  int height_ = 0;
  int stride_bytes_ = 0;
  std::string latest_bgra_;

  SCStream* __strong stream_ = nil;
  SCContentFilter* __strong content_filter_ = nil;
  SCStreamConfiguration* __strong stream_config_ = nil;
  FerrymanStreamOutput* __strong stream_output_ = nil;
  dispatch_queue_t sample_queue_ = nullptr;
};

bool ScreenCaptureKitBridgeImpl::Start(int fps, std::string* error) {
  if (@available(macOS 12.3, *)) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (started_) {
        return true;
      }
    }

    const int safe_fps = std::max(1, std::min(fps, 60));

    __block SCShareableContent* shareable_content = nil;
    __block NSError* shareable_error = nil;
    dispatch_semaphore_t content_semaphore = dispatch_semaphore_create(0);
    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* content, NSError* err) {
      shareable_content = content;
      shareable_error = err;
      dispatch_semaphore_signal(content_semaphore);
    }];

    const dispatch_time_t content_timeout =
        dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(kStartTimeoutMs) * NSEC_PER_MSEC);
    if (dispatch_semaphore_wait(content_semaphore, content_timeout) != 0) {
      if (error != nullptr) {
        *error = "timed out while loading ScreenCaptureKit shareable content";
      }
      return false;
    }

    if (shareable_error != nil) {
      if (error != nullptr) {
        *error = "failed to enumerate displays: " + NSErrorToString(shareable_error);
      }
      return false;
    }

    if (shareable_content == nil || shareable_content.displays.count == 0) {
      if (error != nullptr) {
        *error = "no capture display available";
      }
      return false;
    }

    SCDisplay* display = shareable_content.displays.firstObject;
    if (display == nil) {
      if (error != nullptr) {
        *error = "failed to select target display";
      }
      return false;
    }

    SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    config.width = static_cast<size_t>(display.width);
    config.height = static_cast<size_t>(display.height);
    config.minimumFrameInterval = CMTimeMake(1, safe_fps);
    config.queueDepth = 4;
    config.showsCursor = YES;
    config.pixelFormat = kCVPixelFormatType_32BGRA;

    SCStream* stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:nil];
    if (stream == nil) {
      if (error != nullptr) {
        *error = "failed to initialize ScreenCaptureKit stream";
      }
      return false;
    }

    FerrymanStreamOutput* output = [[FerrymanStreamOutput alloc] initWithOwner:this];
    dispatch_queue_t sample_queue = dispatch_queue_create("dev.ferryman.sckit.sample", DISPATCH_QUEUE_SERIAL);

    NSError* add_output_error = nil;
    if (![stream addStreamOutput:output
                            type:SCStreamOutputTypeScreen
               sampleHandlerQueue:sample_queue
                            error:&add_output_error]) {
      if (error != nullptr) {
        *error = "failed to attach stream output: " + NSErrorToString(add_output_error);
      }
      return false;
    }

    __block NSError* start_error = nil;
    dispatch_semaphore_t start_semaphore = dispatch_semaphore_create(0);
    [stream startCaptureWithCompletionHandler:^(NSError* err) {
      start_error = err;
      dispatch_semaphore_signal(start_semaphore);
    }];

    const dispatch_time_t start_timeout =
        dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(kStartTimeoutMs) * NSEC_PER_MSEC);
    if (dispatch_semaphore_wait(start_semaphore, start_timeout) != 0) {
      if (error != nullptr) {
        *error = "timed out while starting ScreenCaptureKit stream";
      }
      return false;
    }

    if (start_error != nil) {
      if (error != nullptr) {
        *error = "failed to start ScreenCaptureKit stream: " + NSErrorToString(start_error);
      }
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      stream_ = stream;
      content_filter_ = filter;
      stream_config_ = config;
      stream_output_ = output;
      sample_queue_ = sample_queue;

      started_ = true;
      frame_sequence_ = 0;
      consumed_sequence_ = 0;
      width_ = 0;
      height_ = 0;
      stride_bytes_ = 0;
      latest_bgra_.clear();
    }
    return true;
  }

  if (error != nullptr) {
    *error = "ScreenCaptureKit requires macOS 12.3 or newer";
  }
  return false;
}

void ScreenCaptureKitBridgeImpl::Stop() {
  SCStream* stream = nil;
  FerrymanStreamOutput* output = nil;

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_ && stream_ == nil) {
      cv_.notify_all();
      return;
    }
    started_ = false;
    stream = stream_;
    output = stream_output_;
    cv_.notify_all();
  }

  if (@available(macOS 12.3, *)) {
    if (stream != nil && output != nil) {
      NSError* remove_error = nil;
      [stream removeStreamOutput:output type:SCStreamOutputTypeScreen error:&remove_error];
      (void)remove_error;
    }

    if (stream != nil) {
      dispatch_semaphore_t stop_semaphore = dispatch_semaphore_create(0);
      [stream stopCaptureWithCompletionHandler:^(NSError* err) {
        (void)err;
        dispatch_semaphore_signal(stop_semaphore);
      }];

      const dispatch_time_t stop_timeout =
          dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(kStopTimeoutMs) * NSEC_PER_MSEC);
      dispatch_semaphore_wait(stop_semaphore, stop_timeout);
    }
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    stream_ = nil;
    content_filter_ = nil;
    stream_config_ = nil;
    stream_output_ = nil;
    sample_queue_ = nullptr;

    latest_bgra_.clear();
    width_ = 0;
    height_ = 0;
    stride_bytes_ = 0;
    frame_sequence_ = 0;
    consumed_sequence_ = 0;
  }
}

bool ScreenCaptureKitBridgeImpl::WaitForFrame(int timeout_ms, ScreenCaptureKitBridge::RawFrame* frame,
                                              std::string* error) {
  if (frame == nullptr) {
    if (error != nullptr) {
      *error = "frame output is null";
    }
    return false;
  }

  std::unique_lock<std::mutex> lock(mu_);
  const uint64_t last_seen = consumed_sequence_;
  const auto has_new_frame = [this, last_seen]() {
    return frame_sequence_ > last_seen || !started_;
  };

  if (!cv_.wait_for(lock, std::chrono::milliseconds(std::max(timeout_ms, 1)), has_new_frame)) {
    if (error != nullptr) {
      *error = "timed out waiting for ScreenCaptureKit frame";
    }
    return false;
  }

  if (frame_sequence_ <= last_seen) {
    if (error != nullptr) {
      *error = started_ ? "screen frame unavailable" : "screen capture stream stopped";
    }
    return false;
  }

  consumed_sequence_ = frame_sequence_;
  frame->width = width_;
  frame->height = height_;
  frame->stride_bytes = stride_bytes_;
  frame->bgra_bytes = latest_bgra_;
  frame->jpeg_bytes.clear();

  if (frame->bgra_bytes.empty() || frame->stride_bytes <= 0) {
    if (error != nullptr) {
      *error = "received empty ScreenCaptureKit BGRA frame";
    }
    return false;
  }
  return true;
}

void ScreenCaptureKitBridgeImpl::OnSampleBuffer(CMSampleBufferRef sample_buffer,
                                                 SCStreamOutputType type) {
  if (@available(macOS 12.3, *)) {
    if (type != SCStreamOutputTypeScreen || sample_buffer == nullptr || !CMSampleBufferIsValid(sample_buffer)) {
      return;
    }

    CVPixelBufferRef pixel_buffer = CMSampleBufferGetImageBuffer(sample_buffer);
    if (pixel_buffer == nullptr) {
      return;
    }

    CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
    const size_t width = CVPixelBufferGetWidth(pixel_buffer);
    const size_t height = CVPixelBufferGetHeight(pixel_buffer);
    const size_t stride = CVPixelBufferGetBytesPerRow(pixel_buffer);
    void* base_address = CVPixelBufferGetBaseAddress(pixel_buffer);
    if (base_address == nullptr || width == 0 || height == 0 || stride < width * 4) {
      CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
      return;
    }

    std::string bgra(stride * height, '\0');
    for (size_t row = 0; row < height; ++row) {
      const auto* src = static_cast<const uint8_t*>(base_address) + row * stride;
      auto* dst = reinterpret_cast<uint8_t*>(bgra.data()) + row * stride;
      std::memcpy(dst, src, stride);
    }
    CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);

    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!started_) {
        return;
      }
      width_ = static_cast<int>(width);
      height_ = static_cast<int>(height);
      stride_bytes_ = static_cast<int>(stride);
      latest_bgra_ = std::move(bgra);
      ++frame_sequence_;
    }
    cv_.notify_one();
  }
}

ScreenCaptureKitBridge::ScreenCaptureKitBridge() : impl_(new ScreenCaptureKitBridgeImpl()) {}

ScreenCaptureKitBridge::~ScreenCaptureKitBridge() {
  if (impl_ != nullptr) {
    impl_->Stop();
    delete impl_;
    impl_ = nullptr;
  }
}

bool ScreenCaptureKitBridge::Start(int fps, std::string* error) {
  return impl_ != nullptr ? impl_->Start(fps, error) : false;
}

void ScreenCaptureKitBridge::Stop() {
  if (impl_ != nullptr) {
    impl_->Stop();
  }
}

bool ScreenCaptureKitBridge::WaitForFrame(int timeout_ms, RawFrame* frame, std::string* error) {
  return impl_ != nullptr ? impl_->WaitForFrame(timeout_ms, frame, error) : false;
}

}  // namespace ferryman::web

static void FerrymanDispatchSampleBuffer(void* owner, CMSampleBufferRef sample_buffer,
                                         SCStreamOutputType type) {
  if (owner == nullptr) {
    return;
  }
  auto* impl = static_cast<ferryman::web::ScreenCaptureKitBridgeImpl*>(owner);
  impl->OnSampleBuffer(sample_buffer, type);
}

#else

namespace ferryman::web {

class ScreenCaptureKitBridgeImpl {};

ScreenCaptureKitBridge::ScreenCaptureKitBridge() : impl_(new ScreenCaptureKitBridgeImpl()) {}

ScreenCaptureKitBridge::~ScreenCaptureKitBridge() {
  delete impl_;
  impl_ = nullptr;
}

bool ScreenCaptureKitBridge::Start(int fps, std::string* error) {
  (void)fps;
  if (error != nullptr) {
    *error = "ScreenCaptureKit is only supported on macOS";
  }
  return false;
}

void ScreenCaptureKitBridge::Stop() {}

bool ScreenCaptureKitBridge::WaitForFrame(int timeout_ms, RawFrame* frame, std::string* error) {
  (void)timeout_ms;
  (void)frame;
  if (error != nullptr) {
    *error = "ScreenCaptureKit is only supported on macOS";
  }
  return false;
}

}  // namespace ferryman::web

#endif
