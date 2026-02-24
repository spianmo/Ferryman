#pragma once

#include <string>
#include <vector>

namespace ferryman::web {

class ScreenCaptureKitBridgeImpl;

class ScreenCaptureKitBridge {
 public:
  struct DisplayInfo {
    std::string id;
    std::string name;
    int width = 0;
    int height = 0;
    bool is_default = false;
  };

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

  bool Start(int fps, const std::string& display_id, std::string* error);
  void Stop();
  std::vector<DisplayInfo> ListDisplays(std::string* error);
  std::string ActiveDisplayId() const;
  bool WaitForFrame(int timeout_ms, RawFrame* frame, std::string* error);

 private:
  ScreenCaptureKitBridgeImpl* impl_ = nullptr;
};

}  // namespace ferryman::web
