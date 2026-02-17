#include "ferryman/core/ConfigManager.hpp"
#include "ferryman/web/ServerApp.hpp"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>

namespace {

std::atomic<bool> g_running{true};
std::condition_variable g_cv;
std::mutex g_mu;

void SignalHandler(int signal) {
  if (signal == SIGINT || signal == SIGTERM) {
    g_running = false;
    g_cv.notify_all();
  }
}

}  // namespace

int main() {
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  ferryman::core::ConfigManager config_manager;
  if (!config_manager.Initialize()) {
    std::cerr << "[ferryman] failed to initialize config\n";
    return 1;
  }

  ferryman::web::ServerApp app(config_manager.config());
  if (!app.Start()) {
    std::cerr << "[ferryman] failed to start server\n";
    return 1;
  }

  {
    std::unique_lock<std::mutex> lock(g_mu);
    g_cv.wait(lock, [] {
      return !g_running.load();
    });
  }

  app.Stop();
  return 0;
}
