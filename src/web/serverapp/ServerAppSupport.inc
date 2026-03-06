namespace {

using nlohmann::json;
constexpr int kNativeCaptureFps = 8;
constexpr int kNativeCaptureMinFps = 1;
constexpr int kNativeCaptureMaxFps = 60;
constexpr int kNativeScaleMinPercent = 40;
constexpr int kNativeScaleMaxPercent = 100;
constexpr int kNativeScaleDefaultPercent = 75;
constexpr int kNativeBitrateMinBps = 500'000;
constexpr int kNativeBitrateMaxBps = 12'000'000;
constexpr int kNativeBitrateDefaultBps = 3'000'000;
constexpr int kDefaultCodeServerPort = 13337;

constexpr uint32_t kNativeBinaryMagic = 0x314D5246U;  // "FRM1" little-endian
constexpr uint8_t kNativeBinaryCodecJpeg = 1;
constexpr uint8_t kNativeBinaryCodecH264 = 2;
constexpr uint8_t kNativeBinaryCodecH265 = 3;
constexpr uint8_t kNativeBinaryCodecVP8 = 4;
constexpr uint8_t kNativeBinaryCodecVP9 = 5;
constexpr uint8_t kNativeBinaryCodecAV1 = 6;
constexpr int kDockurrSnapshotIntervalMs = 1000;
constexpr int kMonitorSnapshotIntervalMs = 1000;
constexpr int kDockurrCreateLogWaitSeconds = 90;

std::string JsonString(const std::optional<json>& payload, const char* key, const std::string& fallback);
bool JsonBool(const std::optional<json>& payload, const char* key, bool fallback);
int JsonInt(const std::optional<json>& payload, const char* key, int fallback);
std::string ShellEscapeForCommand(const std::string& value);
bool FileExistsAndNonEmpty(const std::filesystem::path& path);
bool EnsureTlsCertificateFiles(const core::AppConfig& config, std::filesystem::path* crt_file,
                               std::filesystem::path* key_file, std::string* error);
#if defined(__unix__) || defined(__APPLE__)
void MarkOpenFdsCloseOnExec();
#endif

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool ParseBool(const std::string& value, bool default_value = false) {
  if (value.empty()) {
    return default_value;
  }
  const std::string lower = ToLower(value);
  return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

std::string CookieValue(const std::string& cookie_header, const std::string& key) {
  if (cookie_header.empty() || key.empty()) {
    return "";
  }
  size_t start = 0;
  while (start < cookie_header.size()) {
    size_t end = cookie_header.find(';', start);
    if (end == std::string::npos) {
      end = cookie_header.size();
    }
    std::string item = util::Trim(cookie_header.substr(start, end - start));
    const size_t eq = item.find('=');
    if (eq != std::string::npos) {
      std::string name = util::Trim(item.substr(0, eq));
      if (name == key) {
        return util::Trim(item.substr(eq + 1));
      }
    }
    start = end + 1;
  }
  return "";
}

int ParseInt(const std::string& value, int default_value) {
  if (value.empty()) {
    return default_value;
  }
  try {
    return std::stoi(value);
  } catch (...) {
    return default_value;
  }
}

std::uint64_t ParseUint64(const std::string& value, std::uint64_t default_value = 0) {
  if (value.empty()) {
    return default_value;
  }
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return default_value;
  }
}

std::vector<std::string> SplitPathSegments(const std::string& path) {
  std::string normalized = path;
  const size_t query_pos = normalized.find('?');
  if (query_pos != std::string::npos) {
    normalized = normalized.substr(0, query_pos);
  }
  const size_t fragment_pos = normalized.find('#');
  if (fragment_pos != std::string::npos) {
    normalized = normalized.substr(0, fragment_pos);
  }

  std::vector<std::string> segments;
  std::string current;
  for (char c : normalized) {
    if (c == '/') {
      if (!current.empty()) {
        segments.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    segments.push_back(current);
  }
  return segments;
}

bool ParseApiSessionPath(const std::string& path, std::string* session_id, std::vector<std::string>* tail_segments) {
  if (session_id == nullptr || tail_segments == nullptr) {
    return false;
  }
  const auto segments = SplitPathSegments(path);
  if (segments.size() < 3 || segments[0] != "api" || segments[1] != "sessions") {
    return false;
  }
  *session_id = segments[2];
  tail_segments->assign(segments.begin() + 3, segments.end());
  return true;
}

bool ParseApiMachinePath(const std::string& path, std::string* machine_id, std::vector<std::string>* tail_segments) {
  if (machine_id == nullptr || tail_segments == nullptr) {
    return false;
  }
  const auto segments = SplitPathSegments(path);
  if (segments.size() < 3 || segments[0] != "api" || segments[1] != "machines") {
    return false;
  }
  *machine_id = segments[2];
  tail_segments->assign(segments.begin() + 3, segments.end());
  return true;
}

bool IsSafeRelativePath(const std::filesystem::path& relative_path) {
  if (relative_path.empty() || relative_path.is_absolute()) {
    return false;
  }
  for (const auto& part : relative_path) {
    if (part == "." || part == "..") {
      return false;
    }
  }
  return true;
}

std::optional<std::filesystem::path> ResolvePathUnderRoot(const std::filesystem::path& root,
                                                          const std::string& raw_relative_path) {
  std::filesystem::path relative = std::filesystem::path(util::Trim(raw_relative_path)).lexically_normal();
  if (relative.empty() || relative == ".") {
    return root;
  }
  if (!IsSafeRelativePath(relative)) {
    return std::nullopt;
  }
  return (root / relative).lexically_normal();
}

bool IsPathInsideRoot(const std::filesystem::path& root, const std::filesystem::path& candidate) {
  std::error_code ec;
  std::filesystem::path normalized_root = std::filesystem::weakly_canonical(root, ec);
  if (ec) {
    return false;
  }
  std::filesystem::path normalized_candidate = std::filesystem::weakly_canonical(candidate, ec);
  if (ec) {
    return false;
  }

  const std::string root_string = normalized_root.generic_string();
  std::string candidate_string = normalized_candidate.generic_string();
  if (!root_string.empty() && root_string.back() != '/') {
    candidate_string = normalized_candidate.generic_string();
  }
  if (candidate_string == root_string) {
    return true;
  }
  if (candidate_string.size() <= root_string.size()) {
    return false;
  }
  if (candidate_string.compare(0, root_string.size(), root_string) != 0) {
    return false;
  }
  return candidate_string[root_string.size()] == '/';
}

std::string ReadFileText(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return "";
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::string RunCommandCaptureWithExitCode(const std::string& command, int* exit_code) {
#if defined(__unix__) || defined(__APPLE__)
  MarkOpenFdsCloseOnExec();
#endif
#if defined(_WIN32)
  FILE* pipe = ::_popen(command.c_str(), "r");
#else
  FILE* pipe = ::popen(command.c_str(), "r");
#endif
  if (pipe == nullptr) {
    if (exit_code != nullptr) {
      *exit_code = -1;
    }
    return "failed to start process";
  }

  std::string output;
  std::array<char, 4096> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output.append(buffer.data());
  }

#if defined(_WIN32)
  int code = ::_pclose(pipe);
#else
  int code = ::pclose(pipe);
  if (WIFEXITED(code)) {
    code = WEXITSTATUS(code);
  }
#endif
  if (exit_code != nullptr) {
    *exit_code = code;
  }
  return output;
}

std::string EscapeForSingleQuotedShell(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('\'');
  for (char c : value) {
    if (c == '\'') {
      escaped += "'\\''";
    } else {
      escaped.push_back(c);
    }
  }
  escaped.push_back('\'');
  return escaped;
}

std::string EscapeForDoubleQuotedShell(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (char c : value) {
    if (c == '"' || c == '\\' || c == '$' || c == '`') {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  escaped.push_back('"');
  return escaped;
}

std::uint64_t EstimateBase64Bytes(const std::string& base64_content) {
  const size_t len = base64_content.size();
  if (len == 0) {
    return 0;
  }
  const size_t padding =
      len >= 2 && base64_content[len - 1] == '=' ? (base64_content[len - 2] == '=' ? 2 : 1) : 0;
  return static_cast<std::uint64_t>((len * 3) / 4 - padding);
}

#if defined(__unix__) || defined(__APPLE__)
void MarkOpenFdsCloseOnExec() {
  long fd_limit = ::sysconf(_SC_OPEN_MAX);
  if (fd_limit <= 0) {
    fd_limit = 4096;
  } else if (fd_limit > 65536) {
    fd_limit = 65536;
  }

  for (int fd = 3; fd < fd_limit; ++fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0 || (flags & FD_CLOEXEC) != 0) {
      continue;
    }
    ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  }
}
#endif

std::string TrimAsciiWhitespace(std::string value) {
  value = util::Trim(value);
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == '\0')) {
    value.pop_back();
  }
  return util::Trim(value);
}

std::string SanitizeUploadFilename(const std::string& name) {
  std::string trimmed = util::Trim(name);
  if (trimmed.empty()) {
    return "upload.bin";
  }
  std::string cleaned;
  cleaned.reserve(trimmed.size());
  for (char c : trimmed) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (uc < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
        c == '>' || c == '|') {
      cleaned.push_back('_');
      continue;
    }
    cleaned.push_back(c);
  }
  if (cleaned.empty()) {
    return "upload.bin";
  }
  return cleaned;
}

std::filesystem::path HomeDirectoryPath() {
#if defined(_WIN32)
  const char* user_profile = std::getenv("USERPROFILE");
  if (user_profile != nullptr && *user_profile != '\0') {
    return std::filesystem::path(user_profile);
  }
  const char* home_drive = std::getenv("HOMEDRIVE");
  const char* home_path = std::getenv("HOMEPATH");
  if (home_drive != nullptr && home_path != nullptr) {
    return std::filesystem::path(std::string(home_drive) + std::string(home_path));
  }
#else
  const char* home = std::getenv("HOME");
  if (home != nullptr && *home != '\0') {
    return std::filesystem::path(home);
  }
#endif
  return std::filesystem::current_path();
}

std::filesystem::path DesktopDirectoryPath() {
#if defined(_WIN32)
  const char* user_profile = std::getenv("USERPROFILE");
  if (user_profile != nullptr && *user_profile != '\0') {
    return std::filesystem::path(user_profile) / "Desktop";
  }
#else
  const char* home = std::getenv("HOME");
  if (home != nullptr && *home != '\0') {
    return std::filesystem::path(home) / "Desktop";
  }
#endif
  return HomeDirectoryPath();
}

std::string RunCommandCapture(const std::string& command) {
#if defined(__unix__) || defined(__APPLE__)
  MarkOpenFdsCloseOnExec();
#endif
#if defined(_WIN32)
  FILE* pipe = ::_popen(command.c_str(), "r");
#else
  FILE* pipe = ::popen(command.c_str(), "r");
#endif
  if (pipe == nullptr) {
    return "";
  }

  std::string output;
  char buffer[2048];
  while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr) {
    output.append(buffer);
  }

#if defined(_WIN32)
  ::_pclose(pipe);
#else
  ::pclose(pipe);
#endif
  return output;
}

int RunCommandNoCapture(const std::string& command) {
  if (command.empty()) {
    return -1;
  }
#if defined(__unix__) || defined(__APPLE__)
  MarkOpenFdsCloseOnExec();
#endif
  return std::system(command.c_str());
}

std::string HostOsTag() {
#if defined(__linux__)
  return "linux";
#elif defined(__FreeBSD__)
  return "freebsd";
#elif defined(__APPLE__)
  return "macos";
#elif defined(_WIN32)
  return "windows";
#else
  return "unknown";
#endif
}

bool CommandExists(const std::string& command) {
  const std::string trimmed = util::Trim(command);
  if (trimmed.empty()) {
    return false;
  }
#if defined(_WIN32)
  const std::string resolved = TrimAsciiWhitespace(RunCommandCapture("where " + trimmed + " 2>nul"));
#else
  const std::string resolved = TrimAsciiWhitespace(RunCommandCapture("command -v " + trimmed + " 2>/dev/null"));
#endif
  return !resolved.empty();
}

bool DetectDockerInstalled() {
  return CommandExists("docker");
}

bool DetectCodeServerInstalled() {
  return CommandExists("code-server");
}

enum class CodeServerTlsMode {
  kFerryman,
  kSelfSigned,
  kCustom,
};

struct CodeServerRuntimeOptions {
  bool https_enabled = false;
  std::vector<std::string> start_args;
  std::string scheme = "http";
  std::string summary = "http";
};

std::string NormalizeCodeServerTlsMode(std::string value, const std::string& fallback = "ferryman") {
  value = ToLower(util::Trim(value));
  if (value == "self-signed" || value == "self_signed") {
    value = "selfsigned";
  }
  if (value == "ferryman" || value == "selfsigned" || value == "custom") {
    return value;
  }
  return NormalizeCodeServerTlsMode(fallback, "ferryman");
}

int NormalizePort(int port, int fallback = kDefaultCodeServerPort) {
  if (port <= 0 || port > 65535) {
    return fallback;
  }
  return port;
}

std::filesystem::path CodeServerPortFilePath() {
  return HomeDirectoryPath() / ".ferryman" / "codeserver_port";
}

std::filesystem::path CodeServerHttpsEnabledFilePath() {
  return HomeDirectoryPath() / ".ferryman" / "codeserver_https_enabled";
}

std::filesystem::path CodeServerTlsModeFilePath() {
  return HomeDirectoryPath() / ".ferryman" / "codeserver_https_mode";
}

std::filesystem::path CodeServerTlsCustomCertFilePath() {
  return HomeDirectoryPath() / ".ferryman" / "codeserver_https_cert_file";
}

std::filesystem::path CodeServerTlsCustomKeyFilePath() {
  return HomeDirectoryPath() / ".ferryman" / "codeserver_https_key_file";
}

std::string LoadSingleLineFileTrimmed(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    return "";
  }
  std::string line;
  std::getline(in, line);
  return util::Trim(line);
}

std::filesystem::path ResolveCodeServerStoredPath(const std::string& raw) {
  const std::string trimmed = util::Trim(raw);
  if (trimmed.empty()) {
    return {};
  }
#if !defined(_WIN32)
  if (trimmed[0] == '~') {
    if (trimmed.size() == 1) {
      return HomeDirectoryPath();
    }
    if (trimmed[1] == '/' || trimmed[1] == '\\') {
      return HomeDirectoryPath() / trimmed.substr(2);
    }
  }
#endif
  std::filesystem::path path(trimmed);
  if (path.is_relative()) {
    return HomeDirectoryPath() / path;
  }
  return path;
}

bool LoadCodeServerHttpsEnabled(const core::AppConfig& config) {
  if (config.has_codeserver_https_enabled) {
    return config.codeserver_https_enabled;
  }
  const std::string raw = LoadSingleLineFileTrimmed(CodeServerHttpsEnabledFilePath());
  if (raw.empty()) {
    return true;
  }
  return ParseBool(raw, true);
}

CodeServerTlsMode LoadCodeServerTlsMode(const core::AppConfig& config) {
  const std::string normalized = config.has_codeserver_https_mode
                                     ? NormalizeCodeServerTlsMode(config.codeserver_https_mode)
                                     : NormalizeCodeServerTlsMode(LoadSingleLineFileTrimmed(CodeServerTlsModeFilePath()));
  if (normalized == "custom") {
    return CodeServerTlsMode::kCustom;
  }
  if (normalized == "selfsigned") {
    return CodeServerTlsMode::kSelfSigned;
  }
  return CodeServerTlsMode::kFerryman;
}

std::string CodeServerTlsModeToString(CodeServerTlsMode mode) {
  switch (mode) {
    case CodeServerTlsMode::kCustom:
      return "custom";
    case CodeServerTlsMode::kSelfSigned:
      return "selfsigned";
    case CodeServerTlsMode::kFerryman:
    default:
      return "ferryman";
  }
}

std::filesystem::path LoadCodeServerCustomCertPath(const core::AppConfig& config) {
  if (config.has_codeserver_https_cert_file) {
    return config.codeserver_https_cert_file;
  }
  return ResolveCodeServerStoredPath(LoadSingleLineFileTrimmed(CodeServerTlsCustomCertFilePath()));
}

std::filesystem::path LoadCodeServerCustomKeyPath(const core::AppConfig& config) {
  if (config.has_codeserver_https_key_file) {
    return config.codeserver_https_key_file;
  }
  return ResolveCodeServerStoredPath(LoadSingleLineFileTrimmed(CodeServerTlsCustomKeyFilePath()));
}

CodeServerRuntimeOptions ResolveCodeServerRuntimeOptions(const core::AppConfig& config) {
  CodeServerRuntimeOptions options;
  options.https_enabled = LoadCodeServerHttpsEnabled(config);
  options.scheme = options.https_enabled ? "https" : "http";
  if (!options.https_enabled) {
    options.summary = "http (https disabled)";
    return options;
  }

  const CodeServerTlsMode mode = LoadCodeServerTlsMode(config);
  if (mode == CodeServerTlsMode::kCustom) {
    const std::filesystem::path cert_path = LoadCodeServerCustomCertPath(config);
    const std::filesystem::path key_path = LoadCodeServerCustomKeyPath(config);
    if (!cert_path.empty() && !key_path.empty() && FileExistsAndNonEmpty(cert_path) && FileExistsAndNonEmpty(key_path)) {
      options.start_args = {"--cert", cert_path.string(), "--cert-key", key_path.string()};
      options.summary = "https mode=custom cert=" + cert_path.string() + " key=" + key_path.string();
      return options;
    }
    options.start_args = {"--cert"};
    options.summary = "https mode=custom but cert/key invalid, fallback=self-signed";
    return options;
  }

  if (mode == CodeServerTlsMode::kSelfSigned) {
    options.start_args = {"--cert"};
    options.summary = "https mode=self-signed";
    return options;
  }

  std::filesystem::path ferryman_crt;
  std::filesystem::path ferryman_key;
  std::string tls_error;
  if (EnsureTlsCertificateFiles(config, &ferryman_crt, &ferryman_key, &tls_error)) {
    options.start_args = {"--cert", ferryman_crt.string(), "--cert-key", ferryman_key.string()};
    options.summary = "https mode=ferryman cert=" + ferryman_crt.string() + " key=" + ferryman_key.string();
    return options;
  }

  options.start_args = {"--cert"};
  options.summary = "https mode=ferryman cert unavailable, fallback=self-signed (" +
                    (tls_error.empty() ? std::string("unknown error") : tls_error) + ")";
  return options;
}

int LoadCodeServerPort(const core::AppConfig& config) {
  if (config.has_codeserver_port && config.codeserver_port > 0 && config.codeserver_port <= 65535) {
    return config.codeserver_port;
  }
  const std::filesystem::path port_file = CodeServerPortFilePath();
  std::ifstream in(port_file);
  if (!in.is_open()) {
    return kDefaultCodeServerPort;
  }
  std::string line;
  std::getline(in, line);
  return NormalizePort(ParseInt(util::Trim(line), kDefaultCodeServerPort), kDefaultCodeServerPort);
}

bool CodeServerHealthProbeSupported() {
#if defined(_WIN32)
  return true;
#else
  return CommandExists("curl") || CommandExists("wget");
#endif
}

bool ProbeCodeServerHealth(int port, bool https_enabled) {
  const int safe_port = NormalizePort(port, kDefaultCodeServerPort);
  const std::string scheme = https_enabled ? "https" : "http";
#if defined(_WIN32)
  const std::string command =
      "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
      "\"" + (https_enabled ? std::string("[System.Net.ServicePointManager]::ServerCertificateValidationCallback = { $true };")
                            : std::string()) +
      "$ErrorActionPreference='SilentlyContinue';"
      "try {"
      "$r=Invoke-WebRequest -UseBasicParsing -Uri '" + scheme + "://127.0.0.1:" +
      std::to_string(safe_port) +
      "/healthz' -TimeoutSec 2;"
      "if ($r.StatusCode -ge 200 -and $r.StatusCode -lt 400) { Write-Output '__ok__' }"
      "} catch {}\"";
  const std::string out = TrimAsciiWhitespace(RunCommandCapture(command));
  return out == "__ok__";
#else
  if (CommandExists("curl")) {
    const std::string command = "curl " + std::string(https_enabled ? "-k " : "") +
                                "-fsS --noproxy '*' --connect-timeout 1 --max-time 2 \"" + scheme +
                                "://127.0.0.1:" + std::to_string(safe_port) + "/healthz\" >/dev/null 2>&1 && echo __ok__";
    const std::string out = TrimAsciiWhitespace(RunCommandCapture(command));
    if (out == "__ok__") {
      return true;
    }
  }
  if (CommandExists("wget")) {
    const std::string command = "wget --no-proxy " + std::string(https_enabled ? "--no-check-certificate " : "") +
                                "-q -T 2 -O - \"" + scheme + "://127.0.0.1:" + std::to_string(safe_port) +
                                "/healthz\" >/dev/null 2>&1 && echo __ok__";
    const std::string out = TrimAsciiWhitespace(RunCommandCapture(command));
    if (out == "__ok__") {
      return true;
    }
  }
  return false;
#endif
}

std::string PowerShellSingleQuoteEscape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (char ch : value) {
    if (ch == '\'') {
      escaped += "''";
      continue;
    }
    escaped.push_back(ch);
  }
  return escaped;
}

std::string BuildCodeServerStartCommand(int port, const std::vector<std::string>& extra_args) {
  const int safe_port = NormalizePort(port, kDefaultCodeServerPort);
#if defined(_WIN32)
  std::string args_literal = "'--bind-addr','0.0.0.0:" + std::to_string(safe_port) + "','--auth','none'";
  for (const auto& arg : extra_args) {
    args_literal += ",'" + PowerShellSingleQuoteEscape(arg) + "'";
  }
  return "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
         "\"$ErrorActionPreference='Stop';"
         "$exe=(Get-Command code-server -ErrorAction Stop).Source;"
         "$args=@(" +
         args_literal +
         ");"
         "Start-Process -FilePath $exe -ArgumentList $args -WindowStyle Hidden\"";
#else
  std::string command = "mkdir -p \"$HOME/.ferryman/logs\" && nohup code-server --bind-addr 0.0.0.0:" +
                        std::to_string(safe_port) + " --auth none";
  for (const auto& arg : extra_args) {
    command += " " + ShellEscapeForCommand(arg);
  }
  command += " > \"$HOME/.ferryman/logs/codeserver.log\" 2>&1 &";
  return command;
#endif
}

std::string BuildCodeServerStopCommand(int port) {
  const int safe_port = NormalizePort(port, kDefaultCodeServerPort);
#if defined(_WIN32)
  return "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
         "\"$ErrorActionPreference='SilentlyContinue';"
         "$pattern='--bind-addr\\s+0\\.0\\.0\\.0:" +
         std::to_string(safe_port) +
         "';"
         "$procs=Get-CimInstance Win32_Process | Where-Object {"
         "$_.Name -match 'code-server' -and $_.CommandLine -match $pattern"
         "};"
         "foreach($p in $procs){"
         "Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue"
         "}\"";
#else
  return "if command -v pkill >/dev/null 2>&1; then "
         "pkill -f " +
         ShellEscapeForCommand("code-server.*--bind-addr 0.0.0.0:" + std::to_string(safe_port)) +
         " >/dev/null 2>&1 || true; fi";
#endif
}

void StopCodeServerProcess(int port) {
  const std::string stop_command = BuildCodeServerStopCommand(port);
  if (stop_command.empty()) {
    return;
  }
  RunCommandNoCapture(stop_command);
}

bool EnsureCodeServerRunning(const core::AppConfig& config, int port, std::string* detail, bool force_restart = false) {
  const int safe_port = NormalizePort(port, kDefaultCodeServerPort);
  const CodeServerRuntimeOptions runtime = ResolveCodeServerRuntimeOptions(config);
  const bool can_probe = CodeServerHealthProbeSupported();

  if (!force_restart && can_probe && ProbeCodeServerHealth(safe_port, runtime.https_enabled)) {
    if (detail != nullptr) {
      *detail = "already running at " + runtime.scheme + "://127.0.0.1:" + std::to_string(safe_port) +
                " (" + runtime.summary + ")";
    }
    return true;
  }

  const std::string start_command = BuildCodeServerStartCommand(safe_port, runtime.start_args);
  if (start_command.empty()) {
    if (detail != nullptr) {
      *detail = "unable to build startup command";
    }
    return false;
  }

  StopCodeServerProcess(safe_port);
  RunCommandNoCapture(start_command);

  if (!can_probe) {
    if (detail != nullptr) {
      *detail = "startup command issued (no health probe available, " + runtime.summary + ")";
    }
    return true;
  }

  for (int attempt = 0; attempt < 8; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (ProbeCodeServerHealth(safe_port, runtime.https_enabled)) {
      if (detail != nullptr) {
        *detail = "started at " + runtime.scheme + "://127.0.0.1:" + std::to_string(safe_port) +
                  " (" + runtime.summary + ")";
      }
      return true;
    }
  }

  if (detail != nullptr) {
    *detail = "startup command issued but health check failed at " + runtime.scheme + "://127.0.0.1:" +
              std::to_string(safe_port) + " (" + runtime.summary + ")";
  }
  return false;
}

bool DockerErrorLooksDaemonInactive(const std::string& error) {
  const std::string lowered = ToLower(util::Trim(error));
  if (lowered.empty()) {
    return false;
  }
  return lowered.find("cannot connect to the docker daemon") != std::string::npos ||
         lowered.find("is the docker daemon running") != std::string::npos ||
         lowered.find("docker daemon is not running") != std::string::npos ||
         lowered.find("error during connect") != std::string::npos ||
         lowered.find("failed to connect to daemon") != std::string::npos ||
         lowered.find("docker.sock") != std::string::npos;
}

bool DetectKvmInstalled() {
#if defined(__linux__)
  std::error_code ec;
  if (std::filesystem::exists("/dev/kvm", ec) && !ec) {
    return true;
  }
  const std::string loaded = TrimAsciiWhitespace(RunCommandCapture("lsmod 2>/dev/null | grep -E '^kvm( |_)' | head -n 1"));
  return !loaded.empty();
#else
  return false;
#endif
}

std::filesystem::path ResolveDropTargetDirectory() {
#if defined(_WIN32)
  const std::string command =
      "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
      "\"$ErrorActionPreference='SilentlyContinue';"
      "Add-Type -Namespace Win32 -Name User32 -MemberDefinition "
      "'[System.Runtime.InteropServices.DllImport(\\\"user32.dll\\\")] public static extern "
      "System.IntPtr GetForegroundWindow();';"
      "$fg=[Win32.User32]::GetForegroundWindow();"
      "$shell=New-Object -ComObject Shell.Application;$path='';"
      "foreach($w in $shell.Windows()){try{if([System.IntPtr]$w.HWND -eq $fg){"
      "$path=$w.Document.Folder.Self.Path;break}}catch{}};"
      "if(-not $path){$path=[Environment]::GetFolderPath('Desktop')};Write-Output $path\"";
  const std::string resolved = TrimAsciiWhitespace(RunCommandCapture(command));
  if (!resolved.empty()) {
    const std::filesystem::path candidate(resolved);
    if (std::filesystem::exists(candidate) && std::filesystem::is_directory(candidate)) {
      return candidate;
    }
  }
#elif defined(__APPLE__)
  const std::string command =
      "/usr/bin/osascript -e 'tell application \"Finder\" to if (count of Finder windows) > 0 then "
      "POSIX path of (target of front window as alias) else POSIX path of (desktop as alias)' 2>/dev/null";
  const std::string resolved = TrimAsciiWhitespace(RunCommandCapture(command));
  if (!resolved.empty()) {
    const std::filesystem::path candidate(resolved);
    if (std::filesystem::exists(candidate) && std::filesystem::is_directory(candidate)) {
      return candidate;
    }
  }
#elif defined(__linux__)
  const std::string resolved = TrimAsciiWhitespace(RunCommandCapture("xdg-user-dir DESKTOP 2>/dev/null"));
  if (!resolved.empty()) {
    const std::filesystem::path candidate(resolved);
    if (std::filesystem::exists(candidate) && std::filesystem::is_directory(candidate)) {
      return candidate;
    }
  }
#endif

  const std::filesystem::path desktop = DesktopDirectoryPath();
  if (std::filesystem::exists(desktop) && std::filesystem::is_directory(desktop)) {
    return desktop;
  }
  return HomeDirectoryPath();
}

std::filesystem::path ResolveUniqueTargetPath(const std::filesystem::path& directory,
                                              const std::string& raw_file_name) {
  const std::string safe_name = SanitizeUploadFilename(raw_file_name);
  const std::filesystem::path name_path(safe_name);
  const std::string stem = name_path.stem().string().empty() ? "upload" : name_path.stem().string();
  const std::string ext = name_path.extension().string();

  std::filesystem::path candidate = directory / safe_name;
  if (!std::filesystem::exists(candidate)) {
    return candidate;
  }

  for (int index = 1; index <= 9999; ++index) {
    candidate = directory / (stem + " (" + std::to_string(index) + ")" + ext);
    if (!std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  return directory / (stem + "-" + util::RandomHex(6) + ext);
}

std::string JsonArray(const std::vector<std::string>& items) {
  std::ostringstream out;
  out << '[';
  for (size_t i = 0; i < items.size(); ++i) {
    out << items[i];
    if (i + 1 < items.size()) {
      out << ',';
    }
  }
  out << ']';
  return out.str();
}

std::string JoinStrings(const std::vector<std::string>& items, const std::string& delimiter) {
  if (items.empty()) {
    return "";
  }
  std::ostringstream out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out << delimiter;
    }
    out << items[i];
  }
  return out.str();
}

std::string ExtractIniKey(const std::string& line) {
  const std::string trimmed = util::Trim(line);
  if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
    return "";
  }
  const size_t eq = trimmed.find('=');
  if (eq == std::string::npos) {
    return "";
  }
  return util::Trim(trimmed.substr(0, eq));
}

std::string NormalizePathForIni(const std::filesystem::path& path) {
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  if (!ec) {
    return absolute.lexically_normal().string();
  }
  return path.lexically_normal().string();
}

bool PathsEqualNormalized(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
  return NormalizePathForIni(lhs) == NormalizePathForIni(rhs);
}

bool UpsertConfigValues(const std::filesystem::path& config_path,
                        const std::vector<std::pair<std::string, std::string>>& key_values, std::string* error) {
  if (config_path.empty()) {
    if (error != nullptr) {
      *error = "empty config path";
    }
    return false;
  }

  std::ifstream input(config_path);
  if (!input.is_open()) {
    if (error != nullptr) {
      *error = "failed to open config file: " + config_path.string();
    }
    return false;
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    lines.push_back(line);
  }
  input.close();

  for (const auto& [key, value] : key_values) {
    bool replaced = false;
    for (auto& existing_line : lines) {
      if (ExtractIniKey(existing_line) == key) {
        existing_line = key + "=" + value;
        replaced = true;
        break;
      }
    }
    if (!replaced) {
      lines.push_back(key + "=" + value);
    }
  }

  std::filesystem::path temp_path = config_path;
  temp_path += ".tmp";
  std::ofstream output(temp_path, std::ios::trunc);
  if (!output.is_open()) {
    if (error != nullptr) {
      *error = "failed to write temp config file: " + temp_path.string();
    }
    return false;
  }
  for (const auto& content_line : lines) {
    output << content_line << '\n';
  }
  output.close();

  std::error_code ec;
  std::filesystem::rename(temp_path, config_path, ec);
  if (!ec) {
    return true;
  }

  ec.clear();
  std::filesystem::copy_file(temp_path, config_path, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "failed to replace config file: " + ec.message();
    }
    std::error_code remove_error;
    std::filesystem::remove(temp_path, remove_error);
    return false;
  }
  std::filesystem::remove(temp_path, ec);
  return true;
}

bool PersistTlsPathsToConfig(const core::AppConfig& config, const std::filesystem::path& crt_path,
                             const std::filesystem::path& key_path, std::string* error) {
  return UpsertConfigValues(config.config_path,
                            {
                                {"tls_cert_file", NormalizePathForIni(crt_path)},
                                {"tls_key_file", NormalizePathForIni(key_path)},
                            },
                            error);
}

bool PersistCodeServerConfigToDisk(const core::AppConfig& config, int port, bool https_enabled,
                                   const std::string& tls_mode, const std::filesystem::path& cert_path,
                                   const std::filesystem::path& key_path, std::string* error) {
  return UpsertConfigValues(config.config_path,
                            {
                                {"codeserver_port", std::to_string(NormalizePort(port, kDefaultCodeServerPort))},
                                {"codeserver_https_enabled", https_enabled ? "true" : "false"},
                                {"codeserver_https_mode", NormalizeCodeServerTlsMode(tls_mode)},
                                {"codeserver_https_cert_file",
                                 cert_path.empty() ? std::string() : NormalizePathForIni(cert_path)},
                                {"codeserver_https_key_file",
                                 key_path.empty() ? std::string() : NormalizePathForIni(key_path)},
                            },
                            error);
}

std::string NormalizeTunnelProtocol(std::string protocol) {
  protocol = ToLower(util::Trim(protocol));
  if (protocol == "udp") {
    return "udp";
  }
  return "tcp";
}

bool TunnelPortValid(int port) {
  return port > 0 && port <= 65535;
}

bool TunnelProxyHostLooksLocal(const std::string& host) {
  const std::string normalized = ToLower(util::Trim(host));
  return normalized == "127.0.0.1" || normalized == "localhost" || normalized == "::1" || normalized == "[::1]" ||
         normalized == "0.0.0.0" || normalized == "::" || normalized == "[::]";
}

std::optional<tunnel::ListeningPortInfo> FindListeningPortConflict(const std::vector<tunnel::ListeningPortInfo>& ports,
                                                                    const std::string& protocol, int port) {
  const std::string normalized_protocol = NormalizeTunnelProtocol(protocol);
  for (const auto& item : ports) {
    if (item.port != port) {
      continue;
    }
    if (NormalizeTunnelProtocol(item.protocol) != normalized_protocol) {
      continue;
    }
    return item;
  }
  return std::nullopt;
}

std::optional<core::TunnelMappingConfig> ParseTunnelMappingFromJson(const std::optional<json>& payload,
                                                                    const std::string& fallback_id = "") {
  if (!payload.has_value() || !payload->is_object()) {
    return std::nullopt;
  }

  core::TunnelMappingConfig mapping;
  mapping.id = util::Trim(JsonString(payload, "id", fallback_id));
  if (mapping.id.empty()) {
    mapping.id = "map-" + util::RandomHex(12);
  }
  mapping.name = util::Trim(JsonString(payload, "name", ""));
  if (mapping.name.empty()) {
    mapping.name = mapping.id;
  }
  mapping.protocol = NormalizeTunnelProtocol(JsonString(payload, "protocol", "tcp"));
  mapping.local_host = util::Trim(JsonString(payload, "local_host", "127.0.0.1"));
  if (mapping.local_host.empty()) {
    mapping.local_host = "127.0.0.1";
  }
  mapping.local_port = JsonInt(payload, "local_port", 0);
  mapping.remote_port = JsonInt(payload, "remote_port", 0);
  mapping.enabled = JsonBool(payload, "enabled", true);

  if (!TunnelPortValid(mapping.local_port) || !TunnelPortValid(mapping.remote_port)) {
    return std::nullopt;
  }
  return mapping;
}

std::string SerializeTunnelMappingConfig(const core::TunnelMappingConfig& mapping) {
  return util::BuildJsonObject({
      {"id", mapping.id, false},
      {"name", mapping.name, false},
      {"protocol", NormalizeTunnelProtocol(mapping.protocol), false},
      {"local_host", mapping.local_host.empty() ? "127.0.0.1" : mapping.local_host, false},
      {"local_port", std::to_string(mapping.local_port), true},
      {"remote_port", std::to_string(mapping.remote_port), true},
      {"enabled", mapping.enabled ? "true" : "false", true},
  });
}

std::string SerializeTunnelMappingsJsonForConfig(const std::vector<core::TunnelMappingConfig>& mappings) {
  json arr = json::array();
  for (const auto& mapping : mappings) {
    arr.push_back({
        {"id", mapping.id},
        {"name", mapping.name},
        {"protocol", NormalizeTunnelProtocol(mapping.protocol)},
        {"local_host", mapping.local_host.empty() ? "127.0.0.1" : mapping.local_host},
        {"local_port", mapping.local_port},
        {"remote_port", mapping.remote_port},
        {"enabled", mapping.enabled},
    });
  }
  return arr.dump();
}

bool PersistTunnelConfigToDisk(const core::AppConfig& config, const std::string& proxy_host, int proxy_port,
                               const std::string& proxy_token, const std::vector<core::TunnelMappingConfig>& mappings,
                               std::string* error) {
  return UpsertConfigValues(config.config_path,
                            {
                                {"tunnel_proxy_host", util::Trim(proxy_host)},
                                {"tunnel_proxy_port", std::to_string(proxy_port)},
                                {"tunnel_proxy_token", util::Trim(proxy_token)},
                                {"tunnel_mappings_json", SerializeTunnelMappingsJsonForConfig(mappings)},
                            },
                            error);
}

std::string ShellEscapeSingleQuoted(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  escaped.push_back('\'');
  for (char ch : value) {
    if (ch == '\'') {
      escaped += "'\\''";
      continue;
    }
    escaped.push_back(ch);
  }
  escaped.push_back('\'');
  return escaped;
}

std::string ShellEscapeForCommand(const std::string& value) {
#if defined(_WIN32)
  std::string escaped;
  escaped.reserve(value.size() + 8);
  escaped.push_back('"');
  for (char ch : value) {
    if (ch == '"') {
      escaped += "\\\"";
      continue;
    }
    escaped.push_back(ch);
  }
  escaped.push_back('"');
  return escaped;
#else
  return ShellEscapeSingleQuoted(value);
#endif
}

int RunShellCommand(const std::string& command) {
  return std::system(command.c_str());
}

std::filesystem::path ResolveTlsDirectory(const core::AppConfig& config) {
  if (!config.config_path.empty()) {
    return config.config_path.parent_path() / "cert";
  }
  return HomeDirectoryPath() / ".ferryman" / "cert";
}

std::filesystem::path ResolveTlsFilePath(const core::AppConfig& config, const std::filesystem::path& configured_path,
                                         const std::filesystem::path& fallback_name) {
  if (!configured_path.empty()) {
    if (configured_path.is_absolute() || config.config_path.empty()) {
      return configured_path;
    }
    return config.config_path.parent_path() / configured_path;
  }
  return ResolveTlsDirectory(config) / fallback_name;
}

bool FileExistsAndNonEmpty(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return false;
  }
  const auto file_size = std::filesystem::file_size(path, ec);
  return !ec && file_size > 0;
}

bool CertificateHasLocalhostSubjectAltName(const std::filesystem::path& crt_path) {
  const std::string escaped_crt = ShellEscapeForCommand(crt_path.string());
#if defined(_WIN32)
  const std::string command = "openssl x509 -noout -ext subjectAltName -in " + escaped_crt + " 2>nul";
#else
  const std::string command = "openssl x509 -noout -ext subjectAltName -in " + escaped_crt + " 2>/dev/null";
#endif
  const std::string output = ToLower(TrimAsciiWhitespace(RunCommandCapture(command)));
  if (output.empty()) {
    return false;
  }
  return output.find("dns:localhost") != std::string::npos || output.find("ip address:127.0.0.1") != std::string::npos ||
         output.find("ip address:0:0:0:0:0:0:0:1") != std::string::npos || output.find("ip:::1") != std::string::npos;
}

bool EnsureTlsCertificateFiles(const core::AppConfig& config, std::filesystem::path* crt_file,
                               std::filesystem::path* key_file, std::string* error) {
  if (crt_file == nullptr || key_file == nullptr) {
    if (error != nullptr) {
      *error = "invalid tls output path";
    }
    return false;
  }

  const bool has_custom_crt = !config.tls_cert_file.empty();
  const bool has_custom_key = !config.tls_key_file.empty();
  if (has_custom_crt != has_custom_key) {
    if (error != nullptr) {
      *error = "tls_cert_file and tls_key_file must be configured together";
    }
    return false;
  }

  const std::filesystem::path default_crt_path = ResolveTlsDirectory(config) / "server.crt";
  const std::filesystem::path default_key_path = ResolveTlsDirectory(config) / "server.key";
  const std::filesystem::path crt_path = ResolveTlsFilePath(config, config.tls_cert_file, "server.crt");
  const std::filesystem::path key_path = ResolveTlsFilePath(config, config.tls_key_file, "server.key");
  const bool uses_default_paths =
      PathsEqualNormalized(crt_path, default_crt_path) && PathsEqualNormalized(key_path, default_key_path);
  const bool use_user_managed_paths = has_custom_crt && !uses_default_paths;

  std::error_code ec;
  if (crt_path.has_parent_path()) {
    std::filesystem::create_directories(crt_path.parent_path(), ec);
    if (ec) {
      if (error != nullptr) {
        *error = "failed to create cert directory: " + ec.message();
      }
      return false;
    }
  }
  ec.clear();
  if (key_path.has_parent_path()) {
    std::filesystem::create_directories(key_path.parent_path(), ec);
    if (ec) {
      if (error != nullptr) {
        *error = "failed to create key directory: " + ec.message();
      }
      return false;
    }
  }

  const bool crt_exists = FileExistsAndNonEmpty(crt_path);
  const bool key_exists = FileExistsAndNonEmpty(key_path);
  bool should_regenerate = false;
  if (crt_exists && key_exists) {
    if (uses_default_paths && !CertificateHasLocalhostSubjectAltName(crt_path)) {
      should_regenerate = true;
    } else {
      *crt_file = crt_path;
      *key_file = key_path;
      return true;
    }
  }

  if (!should_regenerate && crt_exists != key_exists && use_user_managed_paths) {
    if (error != nullptr) {
      *error = "configured tls cert/key must both exist or both be absent";
    }
    return false;
  }

  std::filesystem::remove(crt_path, ec);
  ec.clear();
  std::filesystem::remove(key_path, ec);

  const std::string escaped_crt = ShellEscapeForCommand(crt_path.string());
  const std::string escaped_key = ShellEscapeForCommand(key_path.string());
#if defined(_WIN32)
  const std::string command =
      "openssl genrsa -out " + escaped_key + " 2048 >nul 2>nul && "
      "openssl req -new -x509 -key " +
      escaped_key + " -out " + escaped_crt +
      " -days 3650 -sha256 -subj \"/CN=Ferryman\" "
      "-addext \"subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1\" >nul 2>nul";
#else
  const std::string command =
      "openssl genrsa -out " + escaped_key + " 2048 >/dev/null 2>&1 && "
      "openssl req -new -x509 -key " +
      escaped_key + " -out " + escaped_crt +
      " -days 3650 -sha256 -subj '/CN=Ferryman' "
      "-addext 'subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1' >/dev/null 2>&1";
#endif

  const int rc = RunShellCommand(command);
  if (rc != 0 || !FileExistsAndNonEmpty(crt_path) || !FileExistsAndNonEmpty(key_path) ||
      !CertificateHasLocalhostSubjectAltName(crt_path)) {
    if (error != nullptr) {
      *error = "failed to generate tls certificate/key via openssl (crt=" + crt_path.string() +
               ", key=" + key_path.string() + ")";
    }
    return false;
  }

#if !defined(_WIN32)
  std::filesystem::permissions(key_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, ec);
  (void)ec;
#endif

  *crt_file = crt_path;
  *key_file = key_path;
  return true;
}

std::optional<json> ParseJsonOrNull(const std::string& body) {
  if (body.empty()) {
    return std::nullopt;
  }
  json parsed = json::parse(body, nullptr, false);
  if (parsed.is_discarded()) {
    return std::nullopt;
  }
  return parsed;
}

std::string JsonString(const std::optional<json>& payload, const char* key, const std::string& fallback = "") {
  if (!payload.has_value() || !payload->contains(key)) {
    return fallback;
  }
  const auto& value = (*payload)[key];
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<long long>());
  }
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<unsigned long long>());
  }
  if (value.is_number_float()) {
    return std::to_string(value.get<double>());
  }
  if (value.is_boolean()) {
    return value.get<bool>() ? "true" : "false";
  }
  if (value.is_null()) {
    return fallback;
  }
  return value.dump();
}

bool JsonBool(const std::optional<json>& payload, const char* key, bool fallback = false) {
  if (!payload.has_value() || !payload->contains(key)) {
    return fallback;
  }
  const auto& value = (*payload)[key];
  if (value.is_boolean()) {
    return value.get<bool>();
  }
  if (value.is_number_integer()) {
    return value.get<long long>() != 0;
  }
  if (value.is_string()) {
    return ParseBool(value.get<std::string>(), fallback);
  }
  return fallback;
}

int JsonInt(const std::optional<json>& payload, const char* key, int fallback = 0) {
  if (!payload.has_value() || !payload->contains(key)) {
    return fallback;
  }
  const auto& value = (*payload)[key];
  if (value.is_number_integer()) {
    return value.get<int>();
  }
  if (value.is_number_float()) {
    return static_cast<int>(value.get<double>());
  }
  if (value.is_string()) {
    return ParseInt(value.get<std::string>(), fallback);
  }
  return fallback;
}

std::uint64_t JsonUint64(const std::optional<json>& payload, const char* key, std::uint64_t fallback = 0) {
  if (!payload.has_value() || !payload->contains(key)) {
    return fallback;
  }
  const auto& value = (*payload)[key];
  if (value.is_number_unsigned()) {
    return value.get<std::uint64_t>();
  }
  if (value.is_number_integer()) {
    const long long parsed = value.get<long long>();
    return parsed < 0 ? fallback : static_cast<std::uint64_t>(parsed);
  }
  if (value.is_number_float()) {
    const double parsed = value.get<double>();
    return parsed < 0 ? fallback : static_cast<std::uint64_t>(parsed);
  }
  if (value.is_string()) {
    return ParseUint64(value.get<std::string>(), fallback);
  }
  return fallback;
}

int ParseNativeScalePercent(const std::string& resolution_tier, int fallback) {
  const std::string tier = ToLower(resolution_tier);
  if (tier == "full") {
    return 100;
  }
  if (tier == "balanced") {
    return 75;
  }
  if (tier == "performance") {
    return 50;
  }
  return std::clamp(fallback, kNativeScaleMinPercent, kNativeScaleMaxPercent);
}

std::string NativeResolutionTierFromScale(int scale_percent) {
  if (scale_percent >= 90) {
    return "full";
  }
  if (scale_percent >= 63) {
    return "balanced";
  }
  return "performance";
}

int ParseNativeBitrateBps(const std::string& bitrate_tier, int fallback) {
  const std::string tier = ToLower(bitrate_tier);
  if (tier == "sd") {
    return 1'500'000;
  }
  if (tier == "hd") {
    return 3'000'000;
  }
  if (tier == "uhd") {
    return 6'000'000;
  }
  return std::clamp(fallback, kNativeBitrateMinBps, kNativeBitrateMaxBps);
}

std::string NativeBitrateTierFromBps(int bitrate_bps) {
  if (bitrate_bps >= 4'500'000) {
    return "uhd";
  }
  if (bitrate_bps >= 2'250'000) {
    return "hd";
  }
  return "sd";
}

void AppendUint32Le(std::string* output, uint32_t value) {
  output->push_back(static_cast<char>(value & 0xFF));
  output->push_back(static_cast<char>((value >> 8) & 0xFF));
  output->push_back(static_cast<char>((value >> 16) & 0xFF));
  output->push_back(static_cast<char>((value >> 24) & 0xFF));
}

void AppendUint64Le(std::string* output, uint64_t value) {
  for (int idx = 0; idx < 8; ++idx) {
    output->push_back(static_cast<char>((value >> (idx * 8)) & 0xFF));
  }
}

std::string BuildNativeBinaryFramePacket(uint8_t codec, bool keyframe, uint64_t sequence, int64_t captured_at_ms,
                                         int width, int height, const std::string& payload_bytes) {
  std::string packet;
  packet.reserve(36 + payload_bytes.size());
  AppendUint32Le(&packet, kNativeBinaryMagic);
  packet.push_back(static_cast<char>(codec));
  packet.push_back(static_cast<char>(keyframe ? 1 : 0));
  packet.push_back('\0');
  packet.push_back('\0');
  AppendUint64Le(&packet, sequence);
  AppendUint64Le(&packet, static_cast<uint64_t>(captured_at_ms));
  AppendUint32Le(&packet, static_cast<uint32_t>(std::max(width, 0)));
  AppendUint32Le(&packet, static_cast<uint32_t>(std::max(height, 0)));
  AppendUint32Le(&packet, static_cast<uint32_t>(payload_bytes.size()));
  packet.append(payload_bytes);
  return packet;
}

std::string SerializeDockurrVm(const dockurr::VmInfo& vm) {
  return util::BuildJsonObject({
      {"id", vm.id, false},
      {"name", vm.name, false},
      {"os", vm.os, false},
      {"image", vm.image, false},
      {"state", vm.state, false},
      {"running", vm.running ? "true" : "false", true},
      {"ports", vm.ports, false},
      {"running_for", vm.running_for, false},
      {"persistent", vm.persistent ? "true" : "false", true},
      {"novnc_port", vm.novnc_port, false},
      {"desktop_port", vm.desktop_port, false},
  });
}

std::string FormatDouble(double value) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(3);
  out << value;
  return out.str();
}

std::string SerializeDockerContainer(const docker_runtime::ContainerInfo& container) {
  return util::BuildJsonObject({
      {"id", container.id, false},
      {"name", container.name, false},
      {"image", container.image, false},
      {"state", container.state, false},
      {"status", container.status, false},
      {"running_for", container.running_for, false},
      {"ports", container.ports, false},
      {"created_at", container.created_at, false},
  });
}

std::string SerializeDockerStats(const docker_runtime::ContainerStats& stats) {
  return util::BuildJsonObject({
      {"name", stats.name, false},
      {"cpu_percent", FormatDouble(stats.cpu_percent), true},
      {"memory_usage_bytes", std::to_string(stats.memory_usage_bytes), true},
      {"memory_limit_bytes", std::to_string(stats.memory_limit_bytes), true},
      {"memory_percent", FormatDouble(stats.memory_percent), true},
      {"net_input_bytes", std::to_string(stats.net_input_bytes), true},
      {"net_output_bytes", std::to_string(stats.net_output_bytes), true},
      {"block_input_bytes", std::to_string(stats.block_input_bytes), true},
      {"block_output_bytes", std::to_string(stats.block_output_bytes), true},
      {"pids", std::to_string(stats.pids), true},
  });
}

std::string SerializeDockerFileEntry(const docker_runtime::ContainerFileEntry& entry) {
  return util::BuildJsonObject({
      {"name", entry.name, false},
      {"path", entry.path, false},
      {"is_directory", entry.is_directory ? "true" : "false", true},
      {"size", std::to_string(entry.size), true},
      {"modified_at", std::to_string(entry.modified_at), true},
      {"permissions", entry.permissions, false},
  });
}

std::string SerializeStringArray(const std::vector<std::string>& values) {
  std::vector<std::string> items;
  items.reserve(values.size());
  for (const auto& value : values) {
    items.push_back("\"" + util::JsonEscape(value) + "\"");
  }
  return JsonArray(items);
}

std::string SerializeTunnelSnapshot(const tunnel::TunnelMappingSnapshot& snapshot) {
  return util::BuildJsonObject({
      {"id", snapshot.mapping.id, false},
      {"name", snapshot.mapping.name, false},
      {"protocol", NormalizeTunnelProtocol(snapshot.mapping.protocol), false},
      {"local_host", snapshot.mapping.local_host.empty() ? "127.0.0.1" : snapshot.mapping.local_host, false},
      {"local_port", std::to_string(snapshot.mapping.local_port), true},
      {"remote_port", std::to_string(snapshot.mapping.remote_port), true},
      {"enabled", snapshot.mapping.enabled ? "true" : "false", true},
      {"active", snapshot.runtime.active ? "true" : "false", true},
      {"status", snapshot.runtime.status, false},
      {"detail", snapshot.runtime.detail, false},
      {"updated_at", snapshot.runtime.updated_at, false},
  });
}

std::string BuildTunnelSnapshotPayload(const std::vector<tunnel::TunnelMappingSnapshot>& snapshots) {
  std::vector<std::string> serialized;
  serialized.reserve(snapshots.size());
  for (const auto& snapshot : snapshots) {
    serialized.push_back(SerializeTunnelSnapshot(snapshot));
  }
  return api::Success({
      {"event", "tunnel_snapshot", false},
      {"mappings", JsonArray(serialized), true},
  });
}

std::string BuildDockurrSnapshotPayload(const std::vector<dockurr::VmInfo>& vms) {
  std::vector<std::string> serialized;
  serialized.reserve(vms.size());
  for (const auto& vm : vms) {
    serialized.push_back(SerializeDockurrVm(vm));
  }
  return api::Success({
      {"event", "dockurr_snapshot", false},
      {"vms", JsonArray(serialized), true},
  });
}

std::string BuildDockurrRuntimeLogPayload(const std::string& level, const std::string& action,
                                          const std::string& message, const std::string& request_id = "") {
  return api::Success({
      {"event", "dockurr_runtime_log", false},
      {"ts", util::UtcNowIso8601(), false},
      {"level", level, false},
      {"action", action, false},
      {"message", message, false},
      {"request_id", request_id, false},
  });
}

}  // namespace
