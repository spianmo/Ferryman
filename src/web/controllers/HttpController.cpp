#include "ferryman/web/controllers/HttpController.hpp"

#include "ferryman/api/ResponseUtil.hpp"
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

void HttpController::CleanupScreenUploads() {
#if FERRYMAN_WITH_LIBHV
  std::lock_guard<std::mutex> lock(screen_upload_mu_);
  for (auto& [_, transfer] : screen_uploads_) {
    transfer.stream.close();
    std::error_code remove_error;
    std::filesystem::remove(transfer.temp_path, remove_error);
  }
  screen_uploads_.clear();
#endif
}

#if FERRYMAN_WITH_LIBHV
#include "../serverapp/ServerAppSupport.inc"
#define ServerApp HttpController
#include "../serverapp/ServerAppHttpCommon.inc"
#include "../serverapp/ServerAppHttpSystem.inc"
#include "../serverapp/ServerAppHttpCodeAgent.inc"
#undef ServerApp
#endif

}  // namespace ferryman::web
