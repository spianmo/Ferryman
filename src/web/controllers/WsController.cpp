#include "ferryman/web/controllers/WsController.hpp"

#include "ferryman/api/ResponseUtil.hpp"
#include "ferryman/docker/DockerManager.hpp"
#include "ferryman/tunnel/PortInspector.hpp"
#include "ferryman/util/Random.hpp"
#include "ferryman/util/StringUtil.hpp"
#include "ferryman/util/Time.hpp"
#include "ferryman/web/EmbeddedAssets.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace ferryman::web {

void WsController::ResetNativeCaptureState() {
#if FERRYMAN_WITH_LIBHV
  active_capture_fps_ = 0;
  active_capture_scale_percent_ = 75;
  active_capture_video_bitrate_bps_ = 3'000'000;
#endif
}

#if FERRYMAN_WITH_LIBHV
std::string WsController::HeaderOf(HttpRequest* req, const std::string& key) const {
  auto it = req->headers.find(key);
  if (it == req->headers.end()) {
    return "";
  }
  return it->second;
}
#endif

#if FERRYMAN_WITH_LIBHV
#include "../serverapp/ServerAppSupport.inc"
#define ServerApp WsController
#include "../serverapp/ServerAppWebSocket.inc"
#undef ServerApp
#endif

}  // namespace ferryman::web
