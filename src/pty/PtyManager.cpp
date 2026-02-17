#include "ferryman/pty/PtyManager.hpp"

#include "ferryman/util/Random.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#if defined(__APPLE__)
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#endif

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ferryman::pty {

struct PtyManager::Terminal {
  std::string id;
  std::string owner_token;
  int master_fd = -1;
  pid_t child_pid = -1;
  std::atomic<bool> running{false};
  std::thread reader;
};

PtyManager::~PtyManager() {
  Shutdown();
}

void PtyManager::SetOutputCallback(OutputCallback callback) {
  std::lock_guard<std::mutex> lock(mu_);
  output_callback_ = std::move(callback);
}

std::optional<std::string> PtyManager::CreateTerminal(const std::string& owner_token, int cols, int rows,
                                                      std::string* error) {
#if !defined(__APPLE__) && !defined(__linux__)
  if (error != nullptr) {
    *error = "PTY is unsupported on this platform";
  }
  return std::nullopt;
#else
  if (cols <= 0) {
    cols = 120;
  }
  if (rows <= 0) {
    rows = 30;
  }

  struct winsize ws {};
  ws.ws_col = static_cast<unsigned short>(cols);
  ws.ws_row = static_cast<unsigned short>(rows);

  int master_fd = -1;
  pid_t pid = ::forkpty(&master_fd, nullptr, nullptr, &ws);
  if (pid < 0) {
    if (error != nullptr) {
      *error = std::string("forkpty failed: ") + std::strerror(errno);
    }
    std::clog << "[ferryman][pty][error] create terminal failed: " << std::strerror(errno) << '\n';
    return std::nullopt;
  }

  if (pid == 0) {
    const char* shell = std::getenv("SHELL");
    if (shell == nullptr || shell[0] == '\0') {
      shell = "/bin/bash";
    }
    ::execlp(shell, shell, "-l", nullptr);
    _exit(127);
  }

  int flags = ::fcntl(master_fd, F_GETFL, 0);
  ::fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

  auto terminal = std::make_shared<Terminal>();
  terminal->id = util::RandomHex(12);
  terminal->owner_token = owner_token;
  terminal->master_fd = master_fd;
  terminal->child_pid = pid;
  terminal->running = true;

  {
    std::lock_guard<std::mutex> lock(mu_);
    terminals_[terminal->id] = terminal;
  }

  std::clog << "[ferryman][pty][info] created terminal id=" << terminal->id.substr(0, 10)
            << " cols=" << cols << " rows=" << rows << '\n';

  terminal->reader = std::thread([this, terminal]() {
    ReaderLoop(terminal);
  });

  return terminal->id;
#endif
}

std::shared_ptr<PtyManager::Terminal> PtyManager::FindTerminal(const std::string& owner_token,
                                                               const std::string& terminal_id) {
  auto it = terminals_.find(terminal_id);
  if (it == terminals_.end()) {
    return nullptr;
  }
  if (it->second->owner_token != owner_token) {
    return nullptr;
  }
  return it->second;
}

bool PtyManager::WriteInput(const std::string& owner_token, const std::string& terminal_id,
                            const std::string& data, std::string* error) {
  std::shared_ptr<Terminal> terminal;
  {
    std::lock_guard<std::mutex> lock(mu_);
    terminal = FindTerminal(owner_token, terminal_id);
  }
  if (terminal == nullptr || !terminal->running) {
    if (error != nullptr) {
      *error = "terminal not found";
    }
    return false;
  }

  const ssize_t written = ::write(terminal->master_fd, data.data(), data.size());
  if (written < 0) {
    if (error != nullptr) {
      *error = std::string("write failed: ") + std::strerror(errno);
    }
    std::clog << "[ferryman][pty][error] write failed terminal=" << terminal_id << ": "
              << std::strerror(errno) << '\n';
    return false;
  }
  return true;
}

bool PtyManager::Resize(const std::string& owner_token, const std::string& terminal_id, int cols, int rows,
                        std::string* error) {
  std::shared_ptr<Terminal> terminal;
  {
    std::lock_guard<std::mutex> lock(mu_);
    terminal = FindTerminal(owner_token, terminal_id);
  }
  if (terminal == nullptr || !terminal->running) {
    if (error != nullptr) {
      *error = "terminal not found";
    }
    return false;
  }

  struct winsize ws {};
  ws.ws_col = static_cast<unsigned short>(cols);
  ws.ws_row = static_cast<unsigned short>(rows);
  if (::ioctl(terminal->master_fd, TIOCSWINSZ, &ws) < 0) {
    if (error != nullptr) {
      *error = std::string("resize failed: ") + std::strerror(errno);
    }
    std::clog << "[ferryman][pty][error] resize failed terminal=" << terminal_id << ": "
              << std::strerror(errno) << '\n';
    return false;
  }
  std::clog << "[ferryman][pty][info] resized terminal id=" << terminal_id
            << " cols=" << cols << " rows=" << rows << '\n';
  return true;
}

bool PtyManager::CloseTerminal(const std::string& owner_token, const std::string& terminal_id,
                               std::string* error) {
  std::shared_ptr<Terminal> terminal;
  {
    std::lock_guard<std::mutex> lock(mu_);
    terminal = FindTerminal(owner_token, terminal_id);
    if (terminal != nullptr) {
      terminals_.erase(terminal_id);
    }
  }
  if (terminal == nullptr) {
    if (error != nullptr) {
      *error = "terminal not found";
    }
    std::clog << "[ferryman][pty][warn] close terminal not found id=" << terminal_id << '\n';
    return false;
  }

  terminal->running = false;
  if (terminal->master_fd >= 0) {
    ::close(terminal->master_fd);
    terminal->master_fd = -1;
  }
  if (terminal->child_pid > 0) {
    ::kill(terminal->child_pid, SIGHUP);
    int status = 0;
    ::waitpid(terminal->child_pid, &status, WNOHANG);
  }
  if (terminal->reader.joinable()) {
    terminal->reader.join();
  }
  std::clog << "[ferryman][pty][info] closed terminal id=" << terminal_id << '\n';
  return true;
}

std::vector<std::string> PtyManager::ListTerminals(const std::string& owner_token) {
  std::vector<std::string> ids;
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& [id, terminal] : terminals_) {
    if (terminal->owner_token == owner_token && terminal->running) {
      ids.push_back(id);
    }
  }
  return ids;
}

void PtyManager::ReaderLoop(const std::shared_ptr<Terminal>& terminal) {
  std::array<char, 4096> buffer{};
  while (terminal->running) {
    const ssize_t n = ::read(terminal->master_fd, buffer.data(), buffer.size());
    if (n > 0) {
      OutputCallback cb;
      {
        std::lock_guard<std::mutex> lock(mu_);
        cb = output_callback_;
      }
      if (cb) {
        cb(terminal->id, std::string(buffer.data(), static_cast<size_t>(n)));
      }
      continue;
    }

    if (n == 0) {
      std::clog << "[ferryman][pty][info] terminal EOF id=" << terminal->id << '\n';
      break;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
      continue;
    }
    if (errno == EINTR) {
      continue;
    }
    std::clog << "[ferryman][pty][error] read failed terminal=" << terminal->id
              << ": " << std::strerror(errno) << '\n';
    break;
  }

  terminal->running = false;
  if (terminal->child_pid > 0) {
    int status = 0;
    ::waitpid(terminal->child_pid, &status, WNOHANG);
  }
}

void PtyManager::Shutdown() {
  std::vector<std::shared_ptr<Terminal>> terminals;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [_, terminal] : terminals_) {
      terminals.push_back(terminal);
    }
    terminals_.clear();
  }

  for (auto& terminal : terminals) {
    terminal->running = false;
    if (terminal->master_fd >= 0) {
      ::close(terminal->master_fd);
      terminal->master_fd = -1;
    }
    if (terminal->child_pid > 0) {
      ::kill(terminal->child_pid, SIGHUP);
    }
    if (terminal->reader.joinable()) {
      terminal->reader.join();
    }
  }
}

}  // namespace ferryman::pty
