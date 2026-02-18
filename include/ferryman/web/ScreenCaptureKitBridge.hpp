#pragma once

#include <string>

namespace ferryman::web {

class ScreenCaptureKitBridgeImpl;

class ScreenCaptureKitBridge {
 public:
  struct RawFrame {
    int width = 0;
    int height = 0;
    int stride_bytes = 0;
    std::string bgra_bytes;
    std::string jpeg_bytes;
  };

  ScreenCaptureKitBridge();
  ~ScreenCaptureKitBridge();

  ScreenCaptureKitBridge(const ScreenCaptureKitBridge&) = delete;
  ScreenCaptureKitBridge& operator=(const ScreenCaptureKitBridge&) = delete;

  bool Start(int fps, std::string* error);
  void Stop();
  bool WaitForFrame(int timeout_ms, RawFrame* frame, std::string* error);

 private:
  ScreenCaptureKitBridgeImpl* impl_ = nullptr;
};

}  // namespace ferryman::web
