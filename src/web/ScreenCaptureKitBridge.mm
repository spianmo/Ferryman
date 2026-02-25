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
#include <exception>
#include <ctime>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ferryman::web {
class ScreenCaptureKitBridgeImpl;
}

static void FerrymanDispatchSampleBuffer(void* owner, CMSampleBufferRef sample_buffer,
                                         SCStreamOutputType type);
static void FerrymanDispatchStreamDidStop(void* owner, NSError* error);

@interface FerrymanStreamOutput : NSObject <SCStreamOutput>
- (instancetype)initWithOwner:(void*)owner;
- (void)invalidateOwner;
@end

@interface FerrymanStreamDelegate : NSObject <SCStreamDelegate>
- (instancetype)initWithOwner:(void*)owner;
- (void)invalidateOwner;
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

- (void)invalidateOwner {
  owner_ = nullptr;
}

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
  (void)stream;
  FerrymanDispatchSampleBuffer(owner_, sampleBuffer, type);
}

@end

@implementation FerrymanStreamDelegate {
  void* owner_;
}

- (instancetype)initWithOwner:(void*)owner {
  self = [super init];
  if (self != nil) {
    owner_ = owner;
  }
  return self;
}

- (void)invalidateOwner {
  owner_ = nullptr;
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error API_AVAILABLE(macos(12.3)) {
  (void)stream;
  FerrymanDispatchStreamDidStop(owner_, error);
}

@end

namespace ferryman::web {

namespace {

constexpr int kStartTimeoutMs = 8000;
constexpr int kStopTimeoutMs = 3000;
constexpr uint64_t kFrameLogInterval = 300;
constexpr uint64_t kWaitTimeoutLogInterval = 20;

std::string NsStringToStdString(NSString* value);
std::string NSErrorToString(NSError* error);

std::string TimestampUtcIso8601() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm now_tm{};
#if defined(_WIN32)
  gmtime_s(&now_tm, &now_time);
#else
  gmtime_r(&now_time, &now_tm);
#endif
  char buffer[32]{};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &now_tm) == 0) {
    return "unknown-time";
  }
  return std::string(buffer);
}

std::string DescribeNserror(NSError* error) {
  if (error == nil) {
    return "none";
  }
  std::ostringstream oss;
  oss << "domain=" << NsStringToStdString(error.domain) << ", code=" << static_cast<long long>(error.code)
      << ", desc=" << NSErrorToString(error);
  if (error.userInfo != nil && error.userInfo.count > 0) {
    oss << ", userInfoKeys=" << static_cast<unsigned long long>(error.userInfo.count);
  }
  return oss.str();
}

void LogScreenCaptureKit(const char* level, const std::string& detail) {
  std::cerr << "[ferryman][" << level << "][" << TimestampUtcIso8601() << "][screen.sckit][thread="
            << std::this_thread::get_id() << "] " << detail << '\n';
}

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
  void OnStreamDidStop(NSError* error);

 private:
  mutable std::mutex mu_;
  std::condition_variable cv_;

  bool started_ = false;
  uint64_t frame_sequence_ = 0;
  uint64_t consumed_sequence_ = 0;
  uint64_t wait_timeout_count_ = 0;
  uint64_t dropped_sample_count_ = 0;
  bool first_frame_logged_ = false;
  std::string active_display_id_;

  int width_ = 0;
  int height_ = 0;
  int stride_bytes_ = 0;
  std::string latest_bgra_;

  SCStream* __strong stream_ = nil;
  SCContentFilter* __strong content_filter_ = nil;
  SCStreamConfiguration* __strong stream_config_ = nil;
  FerrymanStreamOutput* __strong stream_output_ = nil;
  FerrymanStreamDelegate* __strong stream_delegate_ = nil;
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
    @try {
      auto fail_start = [&](const std::string& message) -> bool {
        if (error != nullptr) {
          *error = message;
        }
        LogScreenCaptureKit("error", "start failed: " + message + " (requested_display_id=" + display_id +
                                       ", fps=" + std::to_string(fps) + ")");
        return false;
      };

      {
        std::lock_guard<std::mutex> lock(mu_);
        if (started_ && (display_id.empty() || display_id == active_display_id_)) {
          LogScreenCaptureKit("info", "start ignored: stream already running (display_id=" + active_display_id_ +
                                         ", fps=" + std::to_string(fps) + ")");
          return true;
        }
      }

      if (!display_id.empty()) {
        const std::string current_display_id = ActiveDisplayId();
        if (!current_display_id.empty() && current_display_id == display_id) {
          LogScreenCaptureKit("info", "start ignored: requested display is already active (display_id=" +
                                         current_display_id + ")");
          return true;
        }
        if (!current_display_id.empty()) {
          LogScreenCaptureKit("info", "switching display from " + current_display_id + " to " + display_id);
        }
        Stop();
      }

      const int safe_fps = std::max(1, std::min(fps, 60));
      LogScreenCaptureKit("info", "start requested (display_id=" + display_id +
                                      ", requested_fps=" + std::to_string(fps) +
                                      ", safe_fps=" + std::to_string(safe_fps) + ")");

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
        return fail_start("timed out while loading ScreenCaptureKit shareable content");
      }

      if (shareable_error != nil) {
        return fail_start("failed to enumerate displays: " + DescribeNserror(shareable_error));
      }

      if (shareable_content == nil || shareable_content.displays.count == 0) {
        return fail_start("no capture display available");
      }
      LogScreenCaptureKit("info", "shareable displays loaded (count=" +
                                      std::to_string(static_cast<unsigned long long>(shareable_content.displays.count)) +
                                      ")");

      SCDisplay* display = shareable_content.displays.firstObject;
      bool selected_requested_display = false;
      if (!display_id.empty()) {
        for (SCDisplay* candidate in shareable_content.displays) {
          if (candidate == nil) {
            continue;
          }
          if (std::to_string(static_cast<uint64_t>(candidate.displayID)) == display_id) {
            display = candidate;
            selected_requested_display = true;
            break;
          }
        }
      }
      if (display == nil) {
        return fail_start("failed to select target display");
      }

      const std::string selected_display_id = std::to_string(static_cast<uint64_t>(display.displayID));
      LogScreenCaptureKit("info", "selected display (display_id=" + selected_display_id +
                                      ", width=" + std::to_string(static_cast<int>(display.width)) +
                                      ", height=" + std::to_string(static_cast<int>(display.height)) +
                                      ", matched_request=" + (selected_requested_display ? "true" : "false") + ")");

      SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
      SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
      config.width = static_cast<size_t>(display.width);
      config.height = static_cast<size_t>(display.height);
      config.minimumFrameInterval = CMTimeMake(1, safe_fps);
      config.queueDepth = 4;
      config.showsCursor = YES;
      config.pixelFormat = kCVPixelFormatType_32BGRA;

      FerrymanStreamDelegate* delegate = [[FerrymanStreamDelegate alloc] initWithOwner:this];
      SCStream* stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:delegate];
      if (stream == nil) {
        return fail_start("failed to initialize ScreenCaptureKit stream");
      }

      FerrymanStreamOutput* output = [[FerrymanStreamOutput alloc] initWithOwner:this];
      dispatch_queue_t sample_queue = dispatch_queue_create("dev.ferryman.sckit.sample", DISPATCH_QUEUE_SERIAL);

      NSError* add_output_error = nil;
      if (![stream addStreamOutput:output
                              type:SCStreamOutputTypeScreen
                 sampleHandlerQueue:sample_queue
                              error:&add_output_error]) {
        return fail_start("failed to attach stream output: " + DescribeNserror(add_output_error));
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
        return fail_start("timed out while starting ScreenCaptureKit stream");
      }

      if (start_error != nil) {
        return fail_start("failed to start ScreenCaptureKit stream: " + DescribeNserror(start_error));
      }

      {
        std::lock_guard<std::mutex> lock(mu_);
        stream_ = stream;
        content_filter_ = filter;
        stream_config_ = config;
        stream_output_ = output;
        stream_delegate_ = delegate;
        sample_queue_ = sample_queue;

        started_ = true;
        frame_sequence_ = 0;
        consumed_sequence_ = 0;
        wait_timeout_count_ = 0;
        dropped_sample_count_ = 0;
        first_frame_logged_ = false;
        width_ = 0;
        height_ = 0;
        stride_bytes_ = 0;
        latest_bgra_.clear();
        active_display_id_ = selected_display_id;
      }
      const char* queue_label_cstr = dispatch_queue_get_label(sample_queue);
      const std::string queue_label = queue_label_cstr != nullptr ? std::string(queue_label_cstr) : std::string("unknown");
      LogScreenCaptureKit("info", "start completed (display_id=" + selected_display_id +
                                      ", fps=" + std::to_string(safe_fps) +
                                      ", queue_depth=" + std::to_string(static_cast<int>(config.queueDepth)) +
                                      ", sample_queue=" + queue_label + ")");
      return true;
    } @catch (NSException* exception) {
      const std::string reason = exception.reason != nil ? NsStringToStdString(exception.reason) : "unknown";
      const std::string message = "Objective-C exception while starting ScreenCaptureKit stream: " + reason;
      if (error != nullptr) {
        *error = message;
      }
      LogScreenCaptureKit("error", message);
      return false;
    }
  }

  if (error != nullptr) {
    *error = "ScreenCaptureKit requires macOS 12.3 or newer";
  }
  return false;
}

void ScreenCaptureKitBridgeImpl::Stop() {
  SCStream* stream = nil;
  FerrymanStreamOutput* output = nil;
  FerrymanStreamDelegate* delegate = nil;
  std::string previous_display_id;

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_ && stream_ == nil) {
      cv_.notify_all();
      return;
    }
    previous_display_id = active_display_id_;
    started_ = false;
    stream = stream_;
    output = stream_output_;
    delegate = stream_delegate_;
    if (output != nil) {
      [output invalidateOwner];
    }
    if (delegate != nil) {
      [delegate invalidateOwner];
    }
    cv_.notify_all();
  }

  @try {
    if (@available(macOS 12.3, *)) {
      if (stream != nil && output != nil) {
        NSError* remove_error = nil;
        [stream removeStreamOutput:output type:SCStreamOutputTypeScreen error:&remove_error];
        if (remove_error != nil) {
          LogScreenCaptureKit("warn", "remove stream output failed: " + DescribeNserror(remove_error) +
                                          " (display_id=" + previous_display_id + ")");
        }
      }

      if (stream != nil) {
        __block NSError* stop_error = nil;
        dispatch_semaphore_t stop_semaphore = dispatch_semaphore_create(0);
        [stream stopCaptureWithCompletionHandler:^(NSError* err) {
          stop_error = err;
          dispatch_semaphore_signal(stop_semaphore);
        }];

        const dispatch_time_t stop_timeout =
            dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(kStopTimeoutMs) * NSEC_PER_MSEC);
        if (dispatch_semaphore_wait(stop_semaphore, stop_timeout) != 0) {
          LogScreenCaptureKit("warn", "timed out while stopping stream (display_id=" + previous_display_id + ")");
        } else if (stop_error != nil) {
          LogScreenCaptureKit("warn", "stop capture completed with error: " + DescribeNserror(stop_error) +
                                          " (display_id=" + previous_display_id + ")");
        }
      }
    }
  } @catch (NSException* exception) {
    const std::string reason = exception.reason != nil ? NsStringToStdString(exception.reason) : "unknown";
    LogScreenCaptureKit("error", "Objective-C exception while stopping ScreenCaptureKit stream: " + reason +
                                     " (display_id=" + previous_display_id + ")");
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    stream_ = nil;
    content_filter_ = nil;
    stream_config_ = nil;
    stream_output_ = nil;
    stream_delegate_ = nil;
    sample_queue_ = nullptr;

    latest_bgra_.clear();
    width_ = 0;
    height_ = 0;
    stride_bytes_ = 0;
    frame_sequence_ = 0;
    consumed_sequence_ = 0;
    wait_timeout_count_ = 0;
    dropped_sample_count_ = 0;
    first_frame_logged_ = false;
    active_display_id_.clear();
  }
  LogScreenCaptureKit("info", "stream stopped (display_id=" + previous_display_id + ")");
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
    ++wait_timeout_count_;
    const uint64_t timeout_count = wait_timeout_count_;
    const bool should_log = timeout_count == 1 || (timeout_count % kWaitTimeoutLogInterval) == 0;
    const uint64_t current_frame_sequence = frame_sequence_;
    const uint64_t current_consumed_sequence = consumed_sequence_;
    const std::string display_id = active_display_id_;
    lock.unlock();
    if (should_log) {
      LogScreenCaptureKit("warn", "wait frame timeout (count=" + std::to_string(timeout_count) +
                                      ", timeout_ms=" + std::to_string(std::max(timeout_ms, 1)) +
                                      ", display_id=" + display_id +
                                      ", frame_sequence=" + std::to_string(current_frame_sequence) +
                                      ", consumed_sequence=" + std::to_string(current_consumed_sequence) + ")");
    }
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

  if (wait_timeout_count_ > 0) {
    LogScreenCaptureKit("info", "frame flow recovered after timeouts (timeout_count=" +
                                    std::to_string(wait_timeout_count_) +
                                    ", display_id=" + active_display_id_ + ")");
    wait_timeout_count_ = 0;
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
      std::lock_guard<std::mutex> lock(mu_);
      ++dropped_sample_count_;
      if (dropped_sample_count_ == 1 || dropped_sample_count_ % kFrameLogInterval == 0) {
        LogScreenCaptureKit("warn", "dropping invalid sample buffer (type=" + std::to_string(static_cast<int>(type)) +
                                        ", dropped=" + std::to_string(dropped_sample_count_) + ")");
      }
      return;
    }

    CVPixelBufferRef pixel_buffer = CMSampleBufferGetImageBuffer(sample_buffer);
    if (pixel_buffer == nullptr) {
      std::lock_guard<std::mutex> lock(mu_);
      ++dropped_sample_count_;
      if (dropped_sample_count_ == 1 || dropped_sample_count_ % kFrameLogInterval == 0) {
        LogScreenCaptureKit("warn", "dropping sample: image buffer is null (dropped=" +
                                        std::to_string(dropped_sample_count_) + ")");
      }
      return;
    }

    const CVReturn lock_status = CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
    if (lock_status != kCVReturnSuccess) {
      std::lock_guard<std::mutex> lock(mu_);
      ++dropped_sample_count_;
      if (dropped_sample_count_ == 1 || dropped_sample_count_ % kFrameLogInterval == 0) {
        LogScreenCaptureKit("warn", "dropping sample: lock base address failed (status=" +
                                        std::to_string(static_cast<int>(lock_status)) +
                                        ", dropped=" + std::to_string(dropped_sample_count_) + ")");
      }
      return;
    }
    const size_t width = CVPixelBufferGetWidth(pixel_buffer);
    const size_t height = CVPixelBufferGetHeight(pixel_buffer);
    const size_t stride = CVPixelBufferGetBytesPerRow(pixel_buffer);
    void* base_address = CVPixelBufferGetBaseAddress(pixel_buffer);
    if (base_address == nullptr || width == 0 || height == 0 || stride < width * 4) {
      CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
      std::lock_guard<std::mutex> lock(mu_);
      ++dropped_sample_count_;
      if (dropped_sample_count_ == 1 || dropped_sample_count_ % kFrameLogInterval == 0) {
        LogScreenCaptureKit("warn", "dropping sample: invalid pixel buffer layout (width=" +
                                        std::to_string(static_cast<unsigned long long>(width)) +
                                        ", height=" + std::to_string(static_cast<unsigned long long>(height)) +
                                        ", stride=" + std::to_string(static_cast<unsigned long long>(stride)) +
                                        ", dropped=" + std::to_string(dropped_sample_count_) + ")");
      }
      return;
    }

    std::string bgra(stride * height, '\0');
    for (size_t row = 0; row < height; ++row) {
      const auto* src = static_cast<const uint8_t*>(base_address) + row * stride;
      auto* dst = reinterpret_cast<uint8_t*>(bgra.data()) + row * stride;
      std::memcpy(dst, src, stride);
    }
    CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);

    bool should_log_first_frame = false;
    bool should_log_periodic_frame = false;
    uint64_t frame_sequence = 0;
    int frame_width = 0;
    int frame_height = 0;
    int frame_stride = 0;
    std::string display_id;
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
      frame_sequence = frame_sequence_;
      frame_width = width_;
      frame_height = height_;
      frame_stride = stride_bytes_;
      display_id = active_display_id_;
      if (!first_frame_logged_) {
        first_frame_logged_ = true;
        should_log_first_frame = true;
      } else if ((frame_sequence_ % kFrameLogInterval) == 0) {
        should_log_periodic_frame = true;
      }
    }
    if (should_log_first_frame) {
      LogScreenCaptureKit("info", "first frame received (display_id=" + display_id +
                                      ", width=" + std::to_string(frame_width) +
                                      ", height=" + std::to_string(frame_height) +
                                      ", stride=" + std::to_string(frame_stride) + ")");
    } else if (should_log_periodic_frame) {
      LogScreenCaptureKit("debug", "frame heartbeat (display_id=" + display_id +
                                       ", frame_sequence=" + std::to_string(frame_sequence) +
                                       ", width=" + std::to_string(frame_width) +
                                       ", height=" + std::to_string(frame_height) + ")");
    }
    cv_.notify_one();
  }
}

void ScreenCaptureKitBridgeImpl::OnStreamDidStop(NSError* error) {
  bool was_started = false;
  std::string display_id;
  {
    std::lock_guard<std::mutex> lock(mu_);
    was_started = started_;
    started_ = false;
    display_id = active_display_id_;
  }
  cv_.notify_all();
  const char* level = error != nil ? "error" : "warn";
  LogScreenCaptureKit(level, "stream did stop callback (was_started=" + std::string(was_started ? "true" : "false") +
                                ", display_id=" + display_id +
                                ", error={" + DescribeNserror(error) + "})");
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
  @try {
    try {
      impl->OnSampleBuffer(sample_buffer, type);
    } catch (const std::exception& ex) {
      ferryman::web::LogScreenCaptureKit("error",
                                         std::string("sample callback C++ exception: ") + ex.what());
    } catch (...) {
      ferryman::web::LogScreenCaptureKit("error", "sample callback C++ exception: unknown");
    }
  } @catch (NSException* exception) {
    const char* reason = exception.reason != nil ? exception.reason.UTF8String : "unknown";
    ferryman::web::LogScreenCaptureKit("error",
                                       std::string("sample callback ObjC exception: ") + reason);
  }
}

static void FerrymanDispatchStreamDidStop(void* owner, NSError* error) {
  if (owner == nullptr) {
    ferryman::web::LogScreenCaptureKit("warn", "stream did stop callback ignored: owner is null");
    return;
  }
  auto* impl = static_cast<ferryman::web::ScreenCaptureKitBridgeImpl*>(owner);
  @try {
    try {
      impl->OnStreamDidStop(error);
    } catch (const std::exception& ex) {
      ferryman::web::LogScreenCaptureKit("error",
                                         std::string("stream did stop callback C++ exception: ") + ex.what());
    } catch (...) {
      ferryman::web::LogScreenCaptureKit("error", "stream did stop callback C++ exception: unknown");
    }
  } @catch (NSException* exception) {
    const char* reason = exception.reason != nil ? exception.reason.UTF8String : "unknown";
    ferryman::web::LogScreenCaptureKit("error",
                                       std::string("stream did stop callback ObjC exception: ") + reason);
  }
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
