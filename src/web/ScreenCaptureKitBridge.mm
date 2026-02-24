#include "ferryman/web/ScreenCaptureKitBridge.hpp"

#if defined(__APPLE__)

#import <ApplicationServices/ApplicationServices.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOKit/graphics/IOGraphicsLib.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

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

bool ReadUint32CfNumber(CFTypeRef value, uint32_t* out) {
  if (value == nullptr || out == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) {
    return false;
  }
  uint32_t parsed = 0;
  if (!CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt32Type, &parsed)) {
    return false;
  }
  *out = parsed;
  return true;
}

std::string PreferredDisplayName(CFDictionaryRef display_info) {
  if (display_info == nullptr || CFGetTypeID(display_info) != CFDictionaryGetTypeID()) {
    return "";
  }
  CFTypeRef names_value = CFDictionaryGetValue(display_info, CFSTR(kDisplayProductName));
  if (names_value == nullptr || CFGetTypeID(names_value) != CFDictionaryGetTypeID()) {
    return "";
  }
  CFDictionaryRef names = static_cast<CFDictionaryRef>(names_value);
  const CFIndex count = CFDictionaryGetCount(names);
  if (count <= 0) {
    return "";
  }
  std::vector<const void*> values(static_cast<size_t>(count));
  CFDictionaryGetKeysAndValues(names, nullptr, values.data());
  for (const void* value : values) {
    if (value != nullptr && CFGetTypeID(value) == CFStringGetTypeID()) {
      return NsStringToStdString((__bridge NSString*)static_cast<CFStringRef>(value));
    }
  }
  return "";
}

std::string DisplayNameFromSystem(CGDirectDisplayID display_id) {
  CFMutableDictionaryRef matching = IOServiceMatching("IODisplayConnect");
  if (matching == nullptr) {
    return "";
  }

  io_iterator_t iterator = IO_OBJECT_NULL;
  const kern_return_t status = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator);
  if (status != KERN_SUCCESS || iterator == IO_OBJECT_NULL) {
    return "";
  }

  const uint32_t target_vendor = CGDisplayVendorNumber(display_id);
  const uint32_t target_model = CGDisplayModelNumber(display_id);
  const uint32_t target_serial = CGDisplaySerialNumber(display_id);
  std::string name;

  io_service_t service = IO_OBJECT_NULL;
  while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
    CFDictionaryRef info = IODisplayCreateInfoDictionary(service, kIODisplayOnlyPreferredName);
    if (info != nullptr) {
      uint32_t vendor = 0;
      uint32_t model = 0;
      uint32_t serial = 0;
      const bool has_vendor = ReadUint32CfNumber(CFDictionaryGetValue(info, CFSTR(kDisplayVendorID)), &vendor);
      const bool has_model = ReadUint32CfNumber(CFDictionaryGetValue(info, CFSTR(kDisplayProductID)), &model);
      const bool has_serial = ReadUint32CfNumber(CFDictionaryGetValue(info, CFSTR(kDisplaySerialNumber)), &serial);
      const bool serial_matches = target_serial == 0 || !has_serial || serial == target_serial;
      if (has_vendor && has_model && vendor == target_vendor && model == target_model && serial_matches) {
        name = PreferredDisplayName(info);
        CFRelease(info);
        IOObjectRelease(service);
        break;
      }
      CFRelease(info);
    }
    IOObjectRelease(service);
  }
  IOObjectRelease(iterator);
  return name;
}

}  // namespace

class ScreenCaptureKitBridgeImpl {
 public:
  bool Start(int fps, const std::string& display_id, std::string* error);
  void Stop();
  std::vector<ScreenCaptureKitBridge::DisplayInfo> ListDisplays(std::string* error);
  std::string ActiveDisplayId() const;
  bool WaitForFrame(int timeout_ms, ScreenCaptureKitBridge::RawFrame* frame, std::string* error);
  void OnSampleBuffer(CMSampleBufferRef sample_buffer, SCStreamOutputType type);

 private:
  mutable std::mutex mu_;
  std::condition_variable cv_;

  bool started_ = false;
  uint64_t frame_sequence_ = 0;
  uint64_t consumed_sequence_ = 0;
  std::string active_display_id_;

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

std::vector<ScreenCaptureKitBridge::DisplayInfo> ScreenCaptureKitBridgeImpl::ListDisplays(
    std::string* error) {
  std::vector<ScreenCaptureKitBridge::DisplayInfo> displays;
  if (@available(macOS 12.3, *)) {
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
      return displays;
    }

    if (shareable_error != nil) {
      if (error != nullptr) {
        *error = "failed to enumerate displays: " + NSErrorToString(shareable_error);
      }
      return displays;
    }

    if (shareable_content == nil || shareable_content.displays.count == 0) {
      if (error != nullptr) {
        *error = "no capture display available";
      }
      return displays;
    }

    const NSUInteger display_count = shareable_content.displays.count;
    displays.reserve(static_cast<size_t>(display_count));
    for (NSUInteger idx = 0; idx < display_count; ++idx) {
      SCDisplay* display = shareable_content.displays[idx];
      if (display == nil) {
        continue;
      }
      ScreenCaptureKitBridge::DisplayInfo info;
      info.id = std::to_string(static_cast<uint64_t>(display.displayID));
      info.width = static_cast<int>(display.width);
      info.height = static_cast<int>(display.height);
      info.is_default = displays.empty();
      const std::string real_name =
          DisplayNameFromSystem(static_cast<CGDirectDisplayID>(display.displayID));
      if (!real_name.empty()) {
        info.name = real_name + " (" + std::to_string(info.width) + "x" + std::to_string(info.height) + ")";
      } else {
        info.name = "Display " + std::to_string(static_cast<unsigned long long>(idx + 1)) +
                    " (" + std::to_string(info.width) + "x" + std::to_string(info.height) + ")";
      }
      displays.push_back(std::move(info));
    }
    if (displays.empty() && error != nullptr) {
      *error = "no capture display available";
    }
    return displays;
  }

  if (error != nullptr) {
    *error = "ScreenCaptureKit requires macOS 12.3 or newer";
  }
  return displays;
}

std::string ScreenCaptureKitBridgeImpl::ActiveDisplayId() const {
  std::lock_guard<std::mutex> lock(mu_);
  return active_display_id_;
}

bool ScreenCaptureKitBridgeImpl::Start(int fps, const std::string& display_id, std::string* error) {
  if (@available(macOS 12.3, *)) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (started_) {
        if (display_id.empty() || display_id == active_display_id_) {
          return true;
        }
      }
    }

    if (!display_id.empty()) {
      const std::string current_display_id = ActiveDisplayId();
      if (!current_display_id.empty() && current_display_id == display_id) {
        return true;
      }
      Stop();
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
    if (!display_id.empty()) {
      for (SCDisplay* candidate in shareable_content.displays) {
        if (candidate == nil) {
          continue;
        }
        if (std::to_string(static_cast<uint64_t>(candidate.displayID)) == display_id) {
          display = candidate;
          break;
        }
      }
    }
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
      active_display_id_ = std::to_string(static_cast<uint64_t>(display.displayID));
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
    active_display_id_.clear();
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

bool ScreenCaptureKitBridge::Start(int fps, const std::string& display_id, std::string* error) {
  return impl_ != nullptr ? impl_->Start(fps, display_id, error) : false;
}

void ScreenCaptureKitBridge::Stop() {
  if (impl_ != nullptr) {
    impl_->Stop();
  }
}

std::vector<ScreenCaptureKitBridge::DisplayInfo> ScreenCaptureKitBridge::ListDisplays(std::string* error) {
  return impl_ != nullptr ? impl_->ListDisplays(error) : std::vector<DisplayInfo>{};
}

std::string ScreenCaptureKitBridge::ActiveDisplayId() const {
  return impl_ != nullptr ? impl_->ActiveDisplayId() : "";
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

bool ScreenCaptureKitBridge::Start(int fps, const std::string& display_id, std::string* error) {
  (void)fps;
  (void)display_id;
  if (error != nullptr) {
    *error = "ScreenCaptureKit is only supported on macOS";
  }
  return false;
}

void ScreenCaptureKitBridge::Stop() {}

std::vector<ScreenCaptureKitBridge::DisplayInfo> ScreenCaptureKitBridge::ListDisplays(std::string* error) {
  if (error != nullptr) {
    *error = "ScreenCaptureKit is only supported on macOS";
  }
  return {};
}

std::string ScreenCaptureKitBridge::ActiveDisplayId() const {
  return "";
}

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
