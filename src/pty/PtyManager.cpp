#include "ferryman/pty/PtyManager.hpp"

#include "ferryman/util/Random.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <Windows.h>
#endif

#if defined(__APPLE__) || defined(__linux__)
#define FERRYMAN_PTY_POSIX 1
#else
#define FERRYMAN_PTY_POSIX 0
#endif

#if FERRYMAN_PTY_POSIX
#if defined(__APPLE__)
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#endif

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ferryman::pty {

namespace {

#if defined(_WIN32)
std::string WinErrorToString(DWORD code) {
  LPSTR message = nullptr;
  const DWORD flags =
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD size =
      ::FormatMessageA(flags, nullptr, code, 0, reinterpret_cast<LPSTR>(&message), 0, nullptr);
  if (size == 0 || message == nullptr) {
    return "code=" + std::to_string(code);
  }

  std::string text(message, size);
  ::LocalFree(message);
  while (!text.empty() &&
         (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
    text.pop_back();
  }
  return text;
}

std::wstring Utf8ToWide(const std::string& input) {
  if (input.empty()) {
    return {};
  }
  const int needed =
      ::MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
  if (needed <= 0) {
    return std::wstring(input.begin(), input.end());
  }
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  (void)::MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), wide.data(),
                              needed);
  return wide;
}

void SafeCloseHandle(HANDLE* handle) {
  if (handle == nullptr || *handle == nullptr || *handle == INVALID_HANDLE_VALUE) {
    return;
  }
  ::CloseHandle(*handle);
  *handle = nullptr;
}

void SafeClosePseudoConsole(HPCON* pseudo_console) {
  if (pseudo_console == nullptr || *pseudo_console == nullptr) {
    return;
  }
  ::ClosePseudoConsole(*pseudo_console);
  *pseudo_console = nullptr;
}

DWORD HResultToErrorCode(HRESULT hr) {
  if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
    return HRESULT_CODE(hr);
  }
  return static_cast<DWORD>(hr);
}

void StopWindowsProcess(HANDLE process_handle) {
  if (process_handle == nullptr || process_handle == INVALID_HANDLE_VALUE) {
    return;
  }

  DWORD wait_result = ::WaitForSingleObject(process_handle, 50);
  if (wait_result == WAIT_TIMEOUT) {
    (void)::TerminateProcess(process_handle, 1);
    (void)::WaitForSingleObject(process_handle, 1000);
  }
}
#endif

}  // namespace

struct PtyManager::Terminal {
  std::string id;
  std::string owner_token;
#if FERRYMAN_PTY_POSIX
  int master_fd = -1;
  pid_t child_pid = -1;
#elif defined(_WIN32)
  HANDLE input_write = nullptr;
  HANDLE output_read = nullptr;
  HANDLE process_handle = nullptr;
  HPCON pseudo_console = nullptr;
#endif
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
#if defined(_WIN32)
  cols = std::clamp(cols > 0 ? cols : 120, 1, 400);
  rows = std::clamp(rows > 0 ? rows : 30, 1, 200);

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE input_read = nullptr;
  HANDLE input_write = nullptr;
  HANDLE output_read = nullptr;
  HANDLE output_write = nullptr;
  HPCON pseudo_console = nullptr;
  PROCESS_INFORMATION process_info{};
  LPPROC_THREAD_ATTRIBUTE_LIST attr_list = nullptr;
  bool attr_initialized = false;

  auto cleanup = [&]() {
    if (attr_initialized && attr_list != nullptr) {
      ::DeleteProcThreadAttributeList(attr_list);
      attr_initialized = false;
    }
    SafeCloseHandle(&input_read);
    SafeCloseHandle(&input_write);
    SafeCloseHandle(&output_read);
    SafeCloseHandle(&output_write);
    SafeCloseHandle(&process_info.hThread);
    SafeCloseHandle(&process_info.hProcess);
    SafeClosePseudoConsole(&pseudo_console);
  };

  if (!::CreatePipe(&input_read, &input_write, &sa, 0)) {
    if (error != nullptr) {
      *error = "CreatePipe(input) failed: " + WinErrorToString(::GetLastError());
    }
    cleanup();
    return std::nullopt;
  }
  if (!::CreatePipe(&output_read, &output_write, &sa, 0)) {
    if (error != nullptr) {
      *error = "CreatePipe(output) failed: " + WinErrorToString(::GetLastError());
    }
    cleanup();
    return std::nullopt;
  }

  (void)::SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0);
  (void)::SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);

  COORD size{};
  size.X = static_cast<SHORT>(cols);
  size.Y = static_cast<SHORT>(rows);
  const HRESULT create_pseudo_hr = ::CreatePseudoConsole(size, input_read, output_write, 0, &pseudo_console);
  SafeCloseHandle(&input_read);
  SafeCloseHandle(&output_write);
  if (FAILED(create_pseudo_hr)) {
    if (error != nullptr) {
      *error = "CreatePseudoConsole failed: " +
               WinErrorToString(HResultToErrorCode(create_pseudo_hr));
    }
    cleanup();
    return std::nullopt;
  }

  SIZE_T attr_size = 0;
  (void)::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  std::vector<unsigned char> attr_buffer(attr_size);
  attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buffer.data());
  if (!::InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
    if (error != nullptr) {
      *error = "InitializeProcThreadAttributeList failed: " + WinErrorToString(::GetLastError());
    }
    cleanup();
    return std::nullopt;
  }
  attr_initialized = true;

  if (!::UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pseudo_console,
                                   sizeof(pseudo_console), nullptr, nullptr)) {
    if (error != nullptr) {
      *error = "UpdateProcThreadAttribute(PSEUDOCONSOLE) failed: " +
               WinErrorToString(::GetLastError());
    }
    cleanup();
    return std::nullopt;
  }

  const char* shell_env = std::getenv("COMSPEC");
  const std::string shell = (shell_env != nullptr && shell_env[0] != '\0')
                                ? std::string(shell_env)
                                : std::string("C:\\Windows\\System32\\cmd.exe");
  std::wstring shell_w = Utf8ToWide(shell);

  STARTUPINFOEXW startup_info{};
  startup_info.StartupInfo.cb = sizeof(startup_info);
  startup_info.lpAttributeList = attr_list;
  if (!::CreateProcessW(shell_w.c_str(), nullptr, nullptr, nullptr, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                        &startup_info.StartupInfo, &process_info)) {
    if (error != nullptr) {
      *error = "CreateProcessW failed: " + WinErrorToString(::GetLastError());
    }
    cleanup();
    return std::nullopt;
  }

  ::DeleteProcThreadAttributeList(attr_list);
  attr_initialized = false;
  attr_list = nullptr;
  SafeCloseHandle(&process_info.hThread);

  auto terminal = std::make_shared<Terminal>();
  terminal->id = util::RandomHex(12);
  terminal->owner_token = owner_token;
  terminal->input_write = input_write;
  terminal->output_read = output_read;
  terminal->process_handle = process_info.hProcess;
  terminal->pseudo_console = pseudo_console;
  terminal->running = true;

  input_write = nullptr;
  output_read = nullptr;
  process_info.hProcess = nullptr;
  pseudo_console = nullptr;

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
#elif FERRYMAN_PTY_POSIX
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
#else
  if (error != nullptr) {
    *error = "PTY is unsupported on this platform";
  }
  return std::nullopt;
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

#if defined(_WIN32)
  if (terminal->input_write == nullptr) {
    if (error != nullptr) {
      *error = "terminal input channel is unavailable";
    }
    return false;
  }

  size_t offset = 0;
  while (offset < data.size()) {
    const size_t remain = data.size() - offset;
    const size_t chunk = std::min(remain, static_cast<size_t>(std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!::WriteFile(terminal->input_write, data.data() + offset, static_cast<DWORD>(chunk), &written,
                     nullptr)) {
      if (error != nullptr) {
        *error = "write failed: " + WinErrorToString(::GetLastError());
      }
      return false;
    }
    if (written == 0) {
      if (error != nullptr) {
        *error = "write failed: wrote zero bytes";
      }
      return false;
    }
    offset += static_cast<size_t>(written);
  }
  return true;
#elif FERRYMAN_PTY_POSIX
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
#else
  (void)data;
  if (error != nullptr) {
    *error = "PTY is unsupported on this platform";
  }
  return false;
#endif
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

#if defined(_WIN32)
  if (terminal->pseudo_console == nullptr) {
    if (error != nullptr) {
      *error = "terminal pseudo console is unavailable";
    }
    return false;
  }

  cols = std::clamp(cols > 0 ? cols : 120, 1, 400);
  rows = std::clamp(rows > 0 ? rows : 30, 1, 200);
  COORD size{};
  size.X = static_cast<SHORT>(cols);
  size.Y = static_cast<SHORT>(rows);

  const HRESULT hr = ::ResizePseudoConsole(terminal->pseudo_console, size);
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "resize failed: " + WinErrorToString(HResultToErrorCode(hr));
    }
    return false;
  }
  std::clog << "[ferryman][pty][info] resized terminal id=" << terminal_id
            << " cols=" << cols << " rows=" << rows << '\n';
  return true;
#elif FERRYMAN_PTY_POSIX
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
#else
  (void)cols;
  (void)rows;
  if (error != nullptr) {
    *error = "PTY is unsupported on this platform";
  }
  return false;
#endif
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
#if defined(_WIN32)
  SafeCloseHandle(&terminal->input_write);
  StopWindowsProcess(terminal->process_handle);
  SafeClosePseudoConsole(&terminal->pseudo_console);
  SafeCloseHandle(&terminal->output_read);
  if (terminal->reader.joinable()) {
    terminal->reader.join();
  }
  SafeCloseHandle(&terminal->process_handle);
#elif FERRYMAN_PTY_POSIX
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
#else
  if (terminal->reader.joinable()) {
    terminal->reader.join();
  }
#endif
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
#if defined(_WIN32)
  std::array<char, 4096> buffer{};
  while (terminal->running) {
    if (terminal->output_read == nullptr) {
      break;
    }
    DWORD read_bytes = 0;
    const BOOL ok = ::ReadFile(terminal->output_read, buffer.data(), static_cast<DWORD>(buffer.size()),
                               &read_bytes, nullptr);
    if (ok && read_bytes > 0) {
      OutputCallback cb;
      {
        std::lock_guard<std::mutex> lock(mu_);
        cb = output_callback_;
      }
      if (cb) {
        cb(terminal->id, std::string(buffer.data(), static_cast<size_t>(read_bytes)));
      }
      continue;
    }

    if (ok && read_bytes == 0) {
      std::clog << "[ferryman][pty][info] terminal EOF id=" << terminal->id << '\n';
      break;
    }

    const DWORD code = ::GetLastError();
    if (code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED ||
        code == ERROR_INVALID_HANDLE || code == ERROR_OPERATION_ABORTED) {
      std::clog << "[ferryman][pty][info] terminal closed id=" << terminal->id << '\n';
      break;
    }
    std::clog << "[ferryman][pty][error] read failed terminal=" << terminal->id
              << ": " << WinErrorToString(code) << '\n';
    break;
  }
  terminal->running = false;
#elif FERRYMAN_PTY_POSIX
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
#else
  (void)terminal;
#endif
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
#if defined(_WIN32)
    SafeCloseHandle(&terminal->input_write);
    StopWindowsProcess(terminal->process_handle);
    SafeClosePseudoConsole(&terminal->pseudo_console);
    SafeCloseHandle(&terminal->output_read);
    if (terminal->reader.joinable()) {
      terminal->reader.join();
    }
    SafeCloseHandle(&terminal->process_handle);
#elif FERRYMAN_PTY_POSIX
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
#else
    if (terminal->reader.joinable()) {
      terminal->reader.join();
    }
#endif
  }
}

}  // namespace ferryman::pty
