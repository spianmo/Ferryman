#include "ferryman/core/ConfigManager.hpp"
#include "ferryman/web/ServerApp.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <csignal>
#include <iostream>
#include <mutex>

#if defined(__APPLE__) || defined(__linux__)
#include <execinfo.h>
#include <unistd.h>
#endif

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

const char* SignalName(int signal) {
  switch (signal) {
    case SIGABRT:
      return "SIGABRT";
    case SIGSEGV:
      return "SIGSEGV";
#if defined(SIGBUS)
    case SIGBUS:
      return "SIGBUS";
#endif
#if defined(SIGILL)
    case SIGILL:
      return "SIGILL";
#endif
#if defined(SIGFPE)
    case SIGFPE:
      return "SIGFPE";
#endif
    default:
      return "UNKNOWN";
  }
}

void DumpBacktraceIfAvailable() {
#if defined(__APPLE__) || defined(__linux__)
  void* trace[64]{};
  const int frame_count = backtrace(trace, static_cast<int>(sizeof(trace) / sizeof(trace[0])));
  if (frame_count > 0) {
    backtrace_symbols_fd(trace, frame_count, STDERR_FILENO);
  }
#endif
}

[[noreturn]] void TerminateHandler() {
  std::cerr << "[ferryman][fatal] std::terminate called\n";
  if (const std::exception_ptr current = std::current_exception(); current != nullptr) {
    try {
      std::rethrow_exception(current);
    } catch (const std::exception& ex) {
      std::cerr << "[ferryman][fatal] uncaught exception: " << ex.what() << '\n';
    } catch (...) {
      std::cerr << "[ferryman][fatal] uncaught exception: unknown non-std type\n";
    }
  } else {
    std::cerr << "[ferryman][fatal] no active exception object\n";
  }
  DumpBacktraceIfAvailable();
  std::abort();
}

[[noreturn]] void FatalSignalHandler(int signal) {
  std::cerr << "[ferryman][fatal] received signal " << signal << " (" << SignalName(signal) << ")\n";
  DumpBacktraceIfAvailable();
  std::signal(signal, SIG_DFL);
  std::raise(signal);
  std::_Exit(128 + signal);
}

}  // namespace

int main() {
  std::set_terminate(TerminateHandler);
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
  std::signal(SIGABRT, FatalSignalHandler);
#if defined(SIGSEGV)
  std::signal(SIGSEGV, FatalSignalHandler);
#endif
#if defined(SIGBUS)
  std::signal(SIGBUS, FatalSignalHandler);
#endif
#if defined(SIGILL)
  std::signal(SIGILL, FatalSignalHandler);
#endif
#if defined(SIGFPE)
  std::signal(SIGFPE, FatalSignalHandler);
#endif

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
