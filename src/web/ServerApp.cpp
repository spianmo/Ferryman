#include "ferryman/web/ServerApp.hpp"

#include "ferryman/api/ResponseUtil.hpp"
#include "ferryman/tunnel/PortInspector.hpp"
#include "ferryman/util/Random.hpp"
#include "ferryman/util/StringUtil.hpp"
#include "ferryman/util/Time.hpp"
#include "ferryman/web/EmbeddedAssets.hpp"

#include <algorithm>
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

#if FERRYMAN_WITH_LIBHV
#include "hv/hlog.h"
#endif

namespace ferryman::web {

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

std::string HostOsTag() {
#if defined(__linux__)
  return "linux";
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

ServerApp::ServerApp(core::AppConfig config)
    : config_(std::move(config)),
      audit_logger_(config_.audit_log_path),
      file_service_(config_.workspace_root),
      docker_manager_(config_.workspace_root),
      dockurr_manager_(config_.workspace_root),
      tunnel_manager_(&audit_logger_) {
#if FERRYMAN_WITH_LIBHV
  audit_logger_.SetRealtimeCallback([this](const std::string& serialized_entry) {
    BroadcastLogEntry(serialized_entry);
  });
#endif
}

ServerApp::~ServerApp() {
  Stop();
}

bool ServerApp::Start() {
#if !FERRYMAN_WITH_LIBHV
  std::cerr << "[ferryman] libhv is not enabled. Reconfigure with FERRYMAN_WITH_LIBHV=ON.\n";
  return false;
#else
  if (running_.exchange(true)) {
    return true;
  }

  // WebSocket and HTTP share one listener.
  config_.ws_port = config_.http_port;

  tunnel_manager_.Configure(config_.tunnel_proxy_host, config_.tunnel_proxy_port, config_.tunnel_proxy_token,
                            config_.tunnel_mappings);
  tunnel_manager_.Start();

  // Disable libhv file logger to avoid generating libhv.yyyymmdd.log files.
  hlog_set_handler(stderr_logger);

  if (!RegisterHttpRoutes() || !RegisterWsHandlers()) {
    audit_logger_.AppendSystem("error", "server.start", "failed to register HTTP/WS routes");
    tunnel_manager_.Stop();
    running_ = false;
    return false;
  }
  screen_service_.SetEncodingTargets(false, false, false, false, false, false);

  pty_manager_.SetOutputCallback([this](const std::string& terminal_id, const std::string& chunk) {
    BroadcastTerminalOutput(terminal_id, chunk);
  });

  http_server_.service = &http_service_;
  http_server_.ws = &ws_service_;
  std::snprintf(http_server_.host, sizeof(http_server_.host), "%s", config_.http_host.c_str());
  http_server_.port = config_.http_port;
  http_server_.https_port = 0;
  http_server_.ssl_ctx = nullptr;
  http_server_.alloced_ssl_ctx = 0;

  bool https_active = false;
  if (config_.https_enabled) {
    const std::string ssl_backend = hssl_backend();
    audit_logger_.AppendSystem("info", "server.tls", "libhv ssl backend: " + ssl_backend);
    if (!HV_WITH_SSL) {
      audit_logger_.AppendSystem("warn", "server.tls",
                                 "https_enabled=true but libhv was built without SSL/TLS; fallback to http/ws only");
    } else if (ssl_backend == "appletls") {
      // libhv 1.3.3 appletls backend does not load cert/key from hssl_ctx_opt_t for server mode.
      // This causes browser TLS handshakes to fail immediately.
      audit_logger_.AppendSystem(
          "warn", "server.tls",
          "https_enabled=true but libhv ssl backend is appletls; server-side TLS handshake is unstable on this "
          "backend. Rebuild dependencies with libhv[ssl] (OpenSSL) and restart.");
    } else {
      std::filesystem::path tls_crt_file;
      std::filesystem::path tls_key_file;
      std::string tls_error;
      if (!EnsureTlsCertificateFiles(config_, &tls_crt_file, &tls_key_file, &tls_error)) {
        audit_logger_.AppendSystem("warn", "server.tls",
                                   "failed to prepare tls certificate; fallback to http/ws only: " +
                                       (tls_error.empty() ? std::string("unknown error") : tls_error));
      } else {
        const bool should_persist_tls_paths = config_.tls_cert_file.empty() && config_.tls_key_file.empty();
        config_.tls_cert_file = tls_crt_file;
        config_.tls_key_file = tls_key_file;
        if (should_persist_tls_paths) {
          std::string persist_error;
          if (!PersistTlsPathsToConfig(config_, tls_crt_file, tls_key_file, &persist_error)) {
            audit_logger_.AppendSystem("warn", "server.tls",
                                       "failed to persist tls cert/key paths to config: " +
                                           (persist_error.empty() ? std::string("unknown error") : persist_error));
          } else {
            audit_logger_.AppendSystem("info", "server.tls",
                                       "persisted tls cert/key paths to config (crt=" + tls_crt_file.string() +
                                           ", key=" + tls_key_file.string() + ")");
          }
        }

        // libhv backends (e.g. appletls) keep a pointer to hssl_ctx_opt_t instead of deep-copying it.
        // Keep TLS option payload alive for the whole ServerApp lifetime.
        tls_cert_file_storage_ = tls_crt_file.string();
        tls_key_file_storage_ = tls_key_file.string();
        tls_ctx_opt_ = {};
        tls_ctx_opt_.crt_file = tls_cert_file_storage_.c_str();
        tls_ctx_opt_.key_file = tls_key_file_storage_.c_str();
        tls_ctx_opt_.verify_peer = 0;
        tls_ctx_opt_.endpoint = HSSL_SERVER;
        hssl_ctx_t tls_ctx = hssl_ctx_new(&tls_ctx_opt_);
        if (tls_ctx == nullptr) {
          audit_logger_.AppendSystem("warn", "server.tls", "failed to initialize ssl context; fallback to http/ws only");
        } else {
          http_server_.ssl_ctx = tls_ctx;
          http_server_.alloced_ssl_ctx = 1;
          http_server_.https_port = config_.https_port;
          https_active = true;
          audit_logger_.AppendSystem("info", "server.tls",
                                     "enabled tls (crt=" + tls_crt_file.string() + ", key=" + tls_key_file.string() +
                                         ", https_port=" + std::to_string(config_.https_port) +
                                         ", backend=" + ssl_backend + ")");
        }
      }
    }
  }
  config_.https_enabled = https_active;

  http_thread_ = std::thread([this]() {
    http_server_run(&http_server_);
  });

  native_screen_thread_ = std::thread([this]() {
    BroadcastNativeFrames();
  });
  dockurr_thread_ = std::thread([this]() {
    BroadcastDockurrSnapshots();
  });
  monitor_thread_ = std::thread([this]() {
    BroadcastMonitorSnapshots();
  });

  std::cout << "[ferryman] http: http://" << config_.http_host << ':' << config_.http_port << '\n';
  std::cout << "[ferryman] ws:   ws://" << config_.http_host << ':' << config_.ws_port << '\n';
  if (https_active) {
    std::cout << "[ferryman] https: https://" << config_.http_host << ':' << config_.https_port << '\n';
    std::cout << "[ferryman] wss:  wss://" << config_.http_host << ':' << config_.https_port << '\n';
  } else {
    std::cout << "[ferryman] https: disabled\n";
    std::cout << "[ferryman] wss:  disabled\n";
  }

  std::string server_start_message = "http=" + config_.http_host + ":" + std::to_string(config_.http_port) +
                                     ", ws=" + config_.http_host + ":" + std::to_string(config_.ws_port);
  if (https_active) {
    server_start_message += ", https=" + config_.http_host + ":" + std::to_string(config_.https_port) +
                            ", wss=" + config_.http_host + ":" + std::to_string(config_.https_port);
  } else {
    server_start_message += ", https=disabled, wss=disabled";
  }
  audit_logger_.AppendSystem("info", "server.start", server_start_message);
  return true;
#endif
}

void ServerApp::Stop() {
  bool was_running = false;
#if FERRYMAN_WITH_LIBHV
  was_running = running_.exchange(false);
  if (was_running) {
    http_server_stop(&http_server_);

    if (http_thread_.joinable()) {
      http_thread_.join();
    }
  }

  if (native_screen_thread_.joinable()) {
    native_screen_thread_.join();
  }
  if (dockurr_thread_.joinable()) {
    dockurr_thread_.join();
  }
  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }

  {
    std::lock_guard<std::mutex> lock(screen_upload_mu_);
    for (auto& [_, transfer] : screen_uploads_) {
      transfer.stream.close();
      std::error_code remove_error;
      std::filesystem::remove(transfer.temp_path, remove_error);
    }
    screen_uploads_.clear();
  }
#endif
  screen_service_.StopCapture();
#if FERRYMAN_WITH_LIBHV
  active_capture_fps_ = 0;
  active_capture_scale_percent_ = kNativeScaleDefaultPercent;
  active_capture_video_bitrate_bps_ = kNativeBitrateDefaultBps;
#endif
  pty_manager_.Shutdown();
  tunnel_manager_.Stop();
  if (was_running) {
    std::vector<std::string> stopped_names;
    std::string cleanup_error;
    if (!dockurr_manager_.StopTemporaryVms(&stopped_names, &cleanup_error)) {
      if (!cleanup_error.empty()) {
        audit_logger_.AppendSystem("warn", "dockurr.cleanup", cleanup_error);
      }
    } else if (!stopped_names.empty()) {
      audit_logger_.AppendSystem("info", "dockurr.cleanup",
                                 "stopped temporary vm(s): " + JoinStrings(stopped_names, ", "));
    }
  }
  if (was_running) {
    audit_logger_.AppendSystem("info", "server.stop", "runtime shutdown completed");
  }
}

bool ServerApp::RegisterHttpRoutes() {
#if !FERRYMAN_WITH_LIBHV
  return false;
#else
  http_service_.GET("/api/health", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleHealth(req, resp);
  });

  http_service_.POST("/api/auth/login", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleLogin(req, resp);
  });

  http_service_.GET("/api/session/me", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleSessionMe(req, resp);
  });

  http_service_.GET("/api/files/list", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleFileList(req, resp);
  });

  http_service_.GET("/api/files/read", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleFileRead(req, resp);
  });

  http_service_.POST("/api/files/write", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleFileWrite(req, resp);
  });

  http_service_.POST("/api/tasks/start", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleTaskStart(req, resp);
  });

  http_service_.GET("/api/tasks/list", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleTaskList(req, resp);
  });

  http_service_.GET("/api/tasks/get", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleTaskGet(req, resp);
  });

  http_service_.GET("/api/logs/tail", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleLogsTail(req, resp);
  });

  http_service_.GET("/api/dockurr/list", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockurrList(req, resp);
  });

  http_service_.POST("/api/dockurr/create", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockurrCreate(req, resp);
  });

  http_service_.POST("/api/dockurr/start", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockurrStart(req, resp);
  });

  http_service_.POST("/api/dockurr/stop", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockurrStop(req, resp);
  });

  http_service_.POST("/api/dockurr/restart", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockurrRestart(req, resp);
  });

  http_service_.GET("/api/dockurr/logs", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockurrLogs(req, resp);
  });

  http_service_.GET("/api/dockurr/inspect", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockurrInspect(req, resp);
  });

  http_service_.GET("/api/docker/list", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockerList(req, resp);
  });

  http_service_.POST("/api/docker/start", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockerStart(req, resp);
  });

  http_service_.POST("/api/docker/stop", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockerStop(req, resp);
  });

  http_service_.POST("/api/docker/restart", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockerRestart(req, resp);
  });

  http_service_.GET("/api/docker/logs", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockerLogs(req, resp);
  });

  http_service_.GET("/api/docker/inspect", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockerInspect(req, resp);
  });

  http_service_.GET("/api/docker/stats", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockerStats(req, resp);
  });

  http_service_.GET("/api/docker/processes", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockerProcesses(req, resp);
  });

  http_service_.GET("/api/docker/files/list", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockerFileList(req, resp);
  });

  http_service_.GET("/api/docker/files/read", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockerFileRead(req, resp);
  });

  http_service_.POST("/api/docker/files/write", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleDockerFileWrite(req, resp);
  });

  http_service_.GET("/api/screen/capabilities", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleScreenCaps(req, resp);
  });

  http_service_.GET("/api/screen/sources", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleScreenSources(req, resp);
  });

  http_service_.POST("/api/screen/input", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleScreenInput(req, resp);
  });

  http_service_.POST("/api/screen/upload/preflight", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleScreenUploadPreflight(req, resp);
  });

  http_service_.POST("/api/screen/upload/begin", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleScreenUploadBegin(req, resp);
  });

  http_service_.POST("/api/screen/upload/chunk", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleScreenUploadChunk(req, resp);
  });

  http_service_.POST("/api/screen/upload/commit", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleScreenUploadCommit(req, resp);
  });

  http_service_.POST("/api/screen/upload/cancel", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleScreenUploadCancel(req, resp);
  });

  http_service_.GET("/api/tunnel/state", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleTunnelState(req, resp);
  });

  http_service_.POST("/api/tunnel/config", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleTunnelConfigUpdate(req, resp);
  });

  http_service_.POST("/api/tunnel/mapping/upsert", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleTunnelMappingUpsert(req, resp);
  });

  http_service_.POST("/api/tunnel/mapping/delete", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleTunnelMappingDelete(req, resp);
  });

  http_service_.POST("/api/tunnel/mapping/test", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleTunnelMappingTest(req, resp);
  });

  http_service_.GET("/api/tunnel/ports", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleTunnelPorts(req, resp);
  });

  http_service_.GET("/", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleStaticAsset(req, resp);
  });

  http_service_.GET("/index.html", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleStaticAsset(req, resp);
  });

  http_service_.GET("/{asset}", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleStaticAsset(req, resp);
  });

  return true;
#endif
}

bool ServerApp::RegisterWsHandlers() {
#if !FERRYMAN_WITH_LIBHV
  return false;
#else
  ws_service_.onopen = [this](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
    HandleWsOpen(channel, req);
  };

  ws_service_.onmessage = [this](const WebSocketChannelPtr& channel, const std::string& msg) {
    HandleWsMessage(channel, msg);
  };

  ws_service_.onclose = [this](const WebSocketChannelPtr& channel) {
    HandleWsClose(channel);
  };
  return true;
#endif
}

#if FERRYMAN_WITH_LIBHV
int ServerApp::Json(HttpResponse* resp, int status, const std::string& body) const {
  resp->status_code = static_cast<http_status>(status);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  resp->body = body;
  return status;
}

int ServerApp::Text(HttpResponse* resp, int status, const std::string& body,
                    const std::string& content_type) const {
  resp->status_code = static_cast<http_status>(status);
  resp->headers["Content-Type"] = content_type;
  resp->body = body;
  return status;
}

std::string ServerApp::HeaderOf(HttpRequest* req, const std::string& key) const {
  auto it = req->headers.find(key);
  if (it == req->headers.end()) {
    return "";
  }
  return it->second;
}

std::string ServerApp::QueryOf(HttpRequest* req, const std::string& key) const {
  auto it = req->query_params.find(key);
  if (it == req->query_params.end()) {
    return "";
  }
  return it->second;
}

std::optional<core::SessionSnapshot> ServerApp::RequireSession(HttpRequest* req, HttpResponse* resp) {
  std::string token = HeaderOf(req, "X-Session-Token");
  if (token.empty()) {
    token = QueryOf(req, "token");
  }
  if (token.empty()) {
    Json(resp, 401, api::Error("missing session token", "unauthorized"));
    return std::nullopt;
  }

  auto session = session_manager_.GetSession(token);
  if (!session.has_value()) {
    Json(resp, 401, api::Error("invalid session token", "unauthorized"));
    return std::nullopt;
  }
  session_manager_.Touch(token);
  return session;
}

int ServerApp::HandleLogin(HttpRequest* req, HttpResponse* resp) {
  std::string provided = util::Trim(req->body);
  if (!provided.empty() && provided.front() == '{') {
    const auto payload = ParseJsonOrNull(provided);
    provided = JsonString(payload, "access_key", provided);
  }

  if (provided != config_.access_key) {
    return Json(resp, 401, api::Error("invalid access key", "unauthorized"));
  }

  std::string client_ip = req->client_addr.ip;
  if (client_ip.empty()) {
    client_ip = "unknown";
  }

  const std::string token = session_manager_.CreateSession(client_ip);
  audit_logger_.Append(token, "auth.login", "login succeeded");

  return Json(resp, 200, api::Success({
                             {"session_token", token, false},
                             {"ws_port", std::to_string(config_.ws_port), true},
                             {"http_port", std::to_string(config_.http_port), true},
                             {"https_enabled", config_.https_enabled ? "true" : "false", true},
                             {"https_port", std::to_string(config_.https_port), true},
                             {"host", config_.http_host, false},
                         }));
}

int ServerApp::HandleSessionMe(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string host_os = HostOsTag();
  const bool docker_installed = DetectDockerInstalled();
  const bool kvm_installed = host_os == "linux" ? DetectKvmInstalled() : false;

  return Json(resp, 200,
              api::Success({
                  {"session_id", session->session_id, false},
                  {"client_ip", session->client_ip, false},
                  {"created_at", std::to_string(session->created_at), true},
                  {"last_seen_at", std::to_string(session->last_seen_at), true},
                  {"command_authorized", "true", true},
                  {"screen_authorized", "true", true},
                  {"host_os", host_os, false},
                  {"docker_installed", docker_installed ? "true" : "false", true},
                  {"kvm_installed", kvm_installed ? "true" : "false", true},
              }));
}

int ServerApp::HandleFileList(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string request_path = QueryOf(req, "path");
  const auto resolved_path = file_service_.ResolvePath(request_path);
  if (!resolved_path.has_value()) {
    return Json(resp, 400, api::Error("path not allowed"));
  }

  std::string error;
  const auto entries = file_service_.ListDirectory(request_path, &error);
  if (!error.empty()) {
    return Json(resp, 400, api::Error(error));
  }

  const auto root_path = file_service_.ResolvePath("/");
  const std::string root_path_value =
      root_path.has_value() ? root_path->string() : config_.workspace_root.string();

  std::vector<std::string> serialized;
  serialized.reserve(entries.size());
  for (const auto& entry : entries) {
    serialized.push_back(util::BuildJsonObject({
        {"name", entry.name, false},
        {"path", entry.path, false},
        {"is_directory", entry.is_directory ? "true" : "false", true},
        {"size", std::to_string(entry.size), true},
        {"modified_at", std::to_string(entry.modified_at), true},
        {"permissions", entry.permissions, false},
    }));
  }

  return Json(resp, 200, api::Success({
                             {"entries", JsonArray(serialized), true},
                             {"current_path", resolved_path->string(), false},
                             {"root_path", root_path_value, false},
                         }));
}

int ServerApp::HandleFileRead(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string path = QueryOf(req, "path");
  std::string error;
  const auto content = file_service_.ReadFile(path, &error);
  if (!content.has_value()) {
    return Json(resp, 400, api::Error(error.empty() ? "failed to read file" : error));
  }

  audit_logger_.Append(session->token, "file.read", path);
  return Json(resp, 200, api::Success({
                             {"path", path, false},
                             {"content_base64", util::Base64Encode(*content), false},
                         }));
}

int ServerApp::HandleFileWrite(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string path = QueryOf(req, "path");
  const bool base64 = ParseBool(QueryOf(req, "base64"), true);
  const std::string content = base64 ? util::Base64Decode(req->body) : req->body;

  std::string error;
  if (!file_service_.WriteFile(path, content, &error)) {
    return Json(resp, 400, api::Error(error.empty() ? "failed to write file" : error));
  }

  audit_logger_.Append(session->token, "file.write", path);
  return Json(resp, 200, api::Success({
                             {"path", path, false},
                             {"bytes", std::to_string(content.size()), true},
                         }));
}

int ServerApp::HandleTaskStart(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  std::string command = util::Trim(req->body);
  if (!command.empty() && command.front() == '{') {
    const auto payload = ParseJsonOrNull(command);
    command = JsonString(payload, "command", command);
  }

  if (command.empty()) {
    return Json(resp, 400, api::Error("command is required"));
  }

  const std::string task_id = task_manager_.StartTask(session->token, command);
  audit_logger_.Append(session->token, "task.start", command);

  return Json(resp, 200, api::Success({
                             {"task_id", task_id, false},
                         }));
}

int ServerApp::HandleTaskList(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto tasks = task_manager_.ListTasks(session->token);
  std::vector<std::string> serialized;
  serialized.reserve(tasks.size());
  for (const auto& task : tasks) {
    serialized.push_back(util::BuildJsonObject({
        {"task_id", task.task_id, false},
        {"command", task.command, false},
        {"status", task::TaskManager::StatusToString(task.status), false},
        {"exit_code", std::to_string(task.exit_code), true},
        {"created_at", task.created_at, false},
        {"updated_at", task.updated_at, false},
    }));
  }

  return Json(resp, 200, api::Success({
                             {"tasks", JsonArray(serialized), true},
                         }));
}

int ServerApp::HandleTaskGet(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string task_id = QueryOf(req, "task_id");
  auto task = task_manager_.GetTask(session->token, task_id);
  if (!task.has_value()) {
    return Json(resp, 404, api::Error("task not found", "not_found"));
  }

  return Json(resp, 200, api::Success({
                             {"task_id", task->task_id, false},
                             {"command", task->command, false},
                             {"status", task::TaskManager::StatusToString(task->status), false},
                             {"exit_code", std::to_string(task->exit_code), true},
                             {"output_base64", util::Base64Encode(task->output), false},
                             {"created_at", task->created_at, false},
                             {"updated_at", task->updated_at, false},
                         }));
}

int ServerApp::HandleLogsTail(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const int lines = ParseInt(QueryOf(req, "lines"), 200);
  return Json(resp, 200, api::Success({
                             {"items", audit_logger_.Tail(static_cast<size_t>(std::max(1, lines))), true},
                         }));
}

int ServerApp::HandleDockurrList(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  std::string error;
  const auto vms = dockurr_manager_.ListVms(&error);
  if (!error.empty()) {
    return Json(resp, 500, api::Error(error, "dockurr_unavailable"));
  }
  return Json(resp, 200, BuildDockurrSnapshotPayload(vms));
}

int ServerApp::HandleDockurrCreate(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  dockurr::CreateVmRequest create_request;
  create_request.os = ToLower(JsonString(payload, "os"));
  create_request.version = JsonString(payload, "version");
  create_request.ram_size = JsonString(payload, "ram", JsonString(payload, "ram_size", "4G"));
  create_request.disk_size = JsonString(payload, "disk", JsonString(payload, "disk_size", "64G"));
  create_request.name = JsonString(payload, "name");
  create_request.persistent = JsonBool(payload, "persist", JsonBool(payload, "persistent", false));

  if (create_request.os.empty()) {
    return Json(resp, 400, api::Error("os is required"));
  }

  dockurr::VmInfo vm;
  std::string error;
  if (!dockurr_manager_.CreateVm(create_request, &vm, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos ||
                             lowered.find("required") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "dockurr_failed"));
  }

  audit_logger_.Append(session->token, "dockurr.create",
                       vm.name + " (" + create_request.os + " " + create_request.version + ")");
  return Json(resp, 200, api::Success({
                             {"vm", SerializeDockurrVm(vm), true},
                         }));
}

int ServerApp::HandleDockurrStart(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string name = JsonString(payload, "name", QueryOf(req, "name"));
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string error;
  if (!dockurr_manager_.StartVm(name, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "dockurr_failed"));
  }

  audit_logger_.Append(session->token, "dockurr.start", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                         }));
}

int ServerApp::HandleDockurrStop(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string name = JsonString(payload, "name", QueryOf(req, "name"));
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string error;
  if (!dockurr_manager_.StopVm(name, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "dockurr_failed"));
  }

  audit_logger_.Append(session->token, "dockurr.stop", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                         }));
}

int ServerApp::HandleDockurrRestart(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string name = JsonString(payload, "name", QueryOf(req, "name"));
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string error;
  if (!dockurr_manager_.RestartVm(name, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "dockurr_failed"));
  }

  audit_logger_.Append(session->token, "dockurr.restart", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                         }));
}

int ServerApp::HandleDockurrLogs(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string name = QueryOf(req, "name");
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }
  const int tail = ParseInt(QueryOf(req, "tail"), 50);

  std::string logs;
  std::string error;
  if (!dockurr_manager_.GetLogs(name, tail, &logs, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "dockurr_failed"));
  }

  audit_logger_.Append(session->token, "dockurr.logs", name + " (tail=" + std::to_string(tail) + ")");
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"logs", logs, false},
                         }));
}

int ServerApp::HandleDockurrInspect(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string name = QueryOf(req, "name");
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string inspect;
  std::string error;
  if (!dockurr_manager_.InspectVm(name, &inspect, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "dockurr_failed"));
  }

  audit_logger_.Append(session->token, "dockurr.inspect", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"inspect", inspect, false},
                         }));
}

int ServerApp::HandleDockerList(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const bool include_all = ParseBool(QueryOf(req, "all"), true);
  std::string error;
  const auto containers = docker_manager_.ListContainers(include_all, &error);
  if (!error.empty()) {
    return Json(resp, 500, api::Error(error, "docker_unavailable"));
  }

  std::vector<std::string> serialized;
  serialized.reserve(containers.size());
  for (const auto& container : containers) {
    serialized.push_back(SerializeDockerContainer(container));
  }

  return Json(resp, 200, api::Success({
                             {"containers", JsonArray(serialized), true},
                             {"all", include_all ? "true" : "false", true},
                         }));
}

int ServerApp::HandleDockerStart(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string name = JsonString(payload, "name", QueryOf(req, "name"));
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string error;
  if (!docker_manager_.StartContainer(name, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  audit_logger_.Append(session->token, "docker.start", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                         }));
}

int ServerApp::HandleDockerStop(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string name = JsonString(payload, "name", QueryOf(req, "name"));
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string error;
  if (!docker_manager_.StopContainer(name, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  audit_logger_.Append(session->token, "docker.stop", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                         }));
}

int ServerApp::HandleDockerRestart(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string name = JsonString(payload, "name", QueryOf(req, "name"));
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string error;
  if (!docker_manager_.RestartContainer(name, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  audit_logger_.Append(session->token, "docker.restart", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                         }));
}

int ServerApp::HandleDockerLogs(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string name = QueryOf(req, "name");
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }
  const int tail = ParseInt(QueryOf(req, "tail"), 160);

  std::string logs;
  std::string error;
  if (!docker_manager_.GetLogs(name, tail, &logs, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  audit_logger_.Append(session->token, "docker.logs", name + " (tail=" + std::to_string(tail) + ")");
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"logs", logs, false},
                         }));
}

int ServerApp::HandleDockerInspect(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string name = QueryOf(req, "name");
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string inspect;
  std::string error;
  if (!docker_manager_.InspectContainer(name, &inspect, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  audit_logger_.Append(session->token, "docker.inspect", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"inspect", inspect, false},
                         }));
}

int ServerApp::HandleDockerStats(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string name = QueryOf(req, "name");
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  docker_runtime::ContainerStats stats;
  std::string error;
  if (!docker_manager_.GetStats(name, &stats, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"stats", SerializeDockerStats(stats), true},
                         }));
}

int ServerApp::HandleDockerProcesses(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string name = QueryOf(req, "name");
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }
  const int limit = ParseInt(QueryOf(req, "limit"), 120);

  docker_runtime::ContainerProcessSnapshot processes;
  std::string error;
  if (!docker_manager_.GetProcesses(name, limit, &processes, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  std::vector<std::string> serialized_rows;
  serialized_rows.reserve(processes.rows.size());
  for (const auto& row : processes.rows) {
    serialized_rows.push_back(SerializeStringArray(row));
  }

  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"columns", SerializeStringArray(processes.columns), true},
                             {"rows", JsonArray(serialized_rows), true},
                         }));
}

int ServerApp::HandleDockerFileList(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string name = QueryOf(req, "name");
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }
  const std::string path = QueryOf(req, "path");

  std::vector<docker_runtime::ContainerFileEntry> entries;
  std::string current_path;
  std::string error;
  if (!docker_manager_.ListFiles(name, path, &entries, &current_path, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos ||
                             lowered.find("directory") != std::string::npos ||
                             lowered.find("required") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  std::vector<std::string> serialized;
  serialized.reserve(entries.size());
  for (const auto& entry : entries) {
    serialized.push_back(SerializeDockerFileEntry(entry));
  }

  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"entries", JsonArray(serialized), true},
                             {"current_path", current_path, false},
                         }));
}

int ServerApp::HandleDockerFileRead(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string name = QueryOf(req, "name");
  const std::string path = QueryOf(req, "path");
  if (name.empty() || path.empty()) {
    return Json(resp, 400, api::Error("name and path are required"));
  }

  std::string content;
  std::string error;
  if (!docker_manager_.ReadFile(name, path, &content, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos ||
                             lowered.find("path") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  audit_logger_.Append(session->token, "docker.file.read", name + ":" + path);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"path", path, false},
                             {"content_base64", util::Base64Encode(content), false},
                         }));
}

int ServerApp::HandleDockerFileWrite(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string name = JsonString(payload, "name", QueryOf(req, "name"));
  const std::string path = JsonString(payload, "path", QueryOf(req, "path"));
  if (name.empty() || path.empty()) {
    return Json(resp, 400, api::Error("name and path are required"));
  }

  bool base64 = ParseBool(QueryOf(req, "base64"), true);
  std::string raw = req->body;
  if (payload.has_value()) {
    if (payload->contains("content_base64")) {
      raw = JsonString(payload, "content_base64");
      base64 = true;
    } else if (payload->contains("content")) {
      raw = JsonString(payload, "content");
      if (QueryOf(req, "base64").empty()) {
        base64 = false;
      }
    }
  }
  const std::string content = base64 ? util::Base64Decode(raw) : raw;

  std::string error;
  if (!docker_manager_.WriteFile(name, path, content, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos ||
                             lowered.find("path") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  audit_logger_.Append(session->token, "docker.file.write", name + ":" + path);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"path", path, false},
                             {"bytes", std::to_string(content.size()), true},
                         }));
}

int ServerApp::HandleScreenCaps(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  return Json(resp, 200, api::Success({
                             {"capabilities", screen_service_.CapabilitiesJson(), true},
                             {"screen_authorized", "true", true},
                             {"native_capture_running", screen_service_.IsCapturing() ? "true" : "false", true},
                         }));
}

int ServerApp::HandleScreenSources(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  std::string source_error;
  const auto sources = screen_service_.ListCaptureSources(&source_error);
  if (sources.empty()) {
    return Json(resp, 500, api::Error(source_error.empty() ? "no capture source available" : source_error));
  }

  json source_payload = json::array();
  std::string default_source_id;
  for (const auto& source : sources) {
    if (default_source_id.empty() && source.is_default) {
      default_source_id = source.id;
    }
    source_payload.push_back({
        {"id", source.id},
        {"name", source.name},
        {"width", source.width},
        {"height", source.height},
        {"is_default", source.is_default},
    });
  }
  if (default_source_id.empty()) {
    default_source_id = sources.front().id;
  }

  std::string active_source_id = screen_service_.ActiveCaptureSourceId();
  if (active_source_id.empty()) {
    active_source_id = screen_service_.NormalizeCaptureSourceId(default_source_id, nullptr);
  }
  if (active_source_id.empty()) {
    active_source_id = default_source_id;
  }

  return Json(resp, 200, api::Success({
                             {"sources", source_payload.dump(), true},
                             {"default_source_id", default_source_id, false},
                             {"active_source_id", active_source_id, false},
                         }));
}

int ServerApp::HandleScreenInput(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string type = JsonString(payload, "type");
  std::string event_payload = JsonString(payload, "payload");
  if (payload.has_value() && payload->contains("payload") && !(*payload)["payload"].is_string()) {
    event_payload = (*payload)["payload"].dump();
  }
  if (type.empty()) {
    return Json(resp, 400, api::Error("type is required"));
  }

  std::string error;
  if (!screen_service_.InjectInputEvent(session->token, {type, event_payload}, &error)) {
    return Json(resp, 403, api::Error(error, "forbidden"));
  }

  audit_logger_.Append(session->token, "screen.input", type);
  return Json(resp, 200, api::Success({
                             {"accepted", "true", true},
                             {"message", error, false},
                         }));
}

int ServerApp::HandleScreenUploadPreflight(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  if (!payload.has_value() || !payload->is_object()) {
    return Json(resp, 400, api::Error("invalid payload"));
  }
  const auto names_it = payload->find("names");
  if (names_it == payload->end() || !names_it->is_array()) {
    return Json(resp, 400, api::Error("names is required"));
  }

  const std::filesystem::path target_directory = ResolveDropTargetDirectory();
  std::error_code fs_error;
  if (!std::filesystem::exists(target_directory, fs_error)) {
    std::filesystem::create_directories(target_directory, fs_error);
  }
  if (fs_error || !std::filesystem::is_directory(target_directory)) {
    return Json(resp, 500, api::Error("failed to resolve upload target directory"));
  }

  json conflict_names = json::array();
  for (const auto& raw_name_value : *names_it) {
    std::string raw_name;
    if (raw_name_value.is_string()) {
      raw_name = raw_name_value.get<std::string>();
    } else {
      raw_name = raw_name_value.dump();
    }
    const std::string safe_name = SanitizeUploadFilename(raw_name);
    const std::filesystem::path target_path = target_directory / safe_name;
    const bool exists = std::filesystem::exists(target_path, fs_error);
    if (fs_error) {
      return Json(resp, 500, api::Error("failed to check target file state"));
    }
    if (exists) {
      conflict_names.push_back(safe_name);
    }
  }

  return Json(resp, 200, api::Success({
                             {"target_dir", target_directory.string(), false},
                             {"conflicts", conflict_names.dump(), true},
                             {"has_conflicts", conflict_names.empty() ? "false" : "true", true},
                             {"checked_count", std::to_string(names_it->size()), true},
                         }));
}

int ServerApp::HandleScreenUploadBegin(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  std::string transfer_id = util::Trim(JsonString(payload, "transfer_id"));
  if (transfer_id.empty()) {
    transfer_id = util::RandomHex(16);
  }
  const std::string file_name_raw = util::Trim(JsonString(payload, "name"));
  if (file_name_raw.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }
  std::string conflict_strategy = ToLower(util::Trim(JsonString(payload, "conflict_strategy", "keep_both")));
  if (conflict_strategy.empty()) {
    conflict_strategy = "keep_both";
  }
  if (conflict_strategy != "overwrite" && conflict_strategy != "keep_both" && conflict_strategy != "skip") {
    return Json(resp, 400, api::Error("invalid conflict_strategy"));
  }

  const std::filesystem::path target_directory = ResolveDropTargetDirectory();
  std::error_code fs_error;
  if (!std::filesystem::exists(target_directory, fs_error)) {
    std::filesystem::create_directories(target_directory, fs_error);
  }
  if (fs_error || !std::filesystem::is_directory(target_directory)) {
    return Json(resp, 500, api::Error("failed to resolve upload target directory"));
  }

  const std::string requested_safe_name = SanitizeUploadFilename(file_name_raw);
  const std::filesystem::path desired_path = target_directory / requested_safe_name;
  const bool desired_exists = std::filesystem::exists(desired_path, fs_error);
  if (fs_error) {
    return Json(resp, 500, api::Error("failed to check target file state"));
  }
  if (desired_exists && std::filesystem::is_directory(desired_path, fs_error)) {
    return Json(resp, 400, api::Error("target path is a directory"));
  }
  if (conflict_strategy == "skip" && desired_exists) {
    return Json(resp, 200, api::Success({
                               {"transfer_id", transfer_id, false},
                               {"target_dir", target_directory.string(), false},
                               {"path", desired_path.string(), false},
                               {"requested_name", requested_safe_name, false},
                               {"saved_name", requested_safe_name, false},
                               {"name_conflict", "true", true},
                               {"skip_existing", "true", true},
                               {"accepted", "true", true},
                           }));
  }

  std::filesystem::path temp_root = std::filesystem::temp_directory_path(fs_error) / "ferryman-screen-upload";
  if (fs_error) {
    temp_root = std::filesystem::current_path() / ".ferryman-screen-upload";
    fs_error.clear();
  }
  std::filesystem::create_directories(temp_root, fs_error);
  if (fs_error) {
    return Json(resp, 500, api::Error("failed to prepare temporary upload directory"));
  }

  ScreenUploadTransfer transfer;
  transfer.owner_session_token = session->token;
  transfer.transfer_id = transfer_id;
  transfer.file_name = file_name_raw;
  transfer.conflict_strategy = conflict_strategy;
  transfer.target_directory = target_directory;
  transfer.expected_bytes = JsonUint64(payload, "size", 0);
  transfer.temp_path = temp_root / (util::RandomHex(16) + ".part");
  transfer.stream.open(transfer.temp_path, std::ios::binary | std::ios::trunc);
  if (!transfer.stream.is_open()) {
    return Json(resp, 500, api::Error("failed to open temporary upload file"));
  }

  const std::string map_key = session->token + ":" + transfer_id;
  {
    std::lock_guard<std::mutex> lock(screen_upload_mu_);
    if (screen_uploads_.find(map_key) != screen_uploads_.end()) {
      transfer.stream.close();
      std::filesystem::remove(transfer.temp_path, fs_error);
      return Json(resp, 409, api::Error("transfer already exists", "conflict"));
    }
    screen_uploads_.emplace(map_key, std::move(transfer));
  }

  return Json(resp, 200, api::Success({
                             {"transfer_id", transfer_id, false},
                             {"target_dir", target_directory.string(), false},
                             {"path", desired_path.string(), false},
                             {"requested_name", requested_safe_name, false},
                             {"saved_name", requested_safe_name, false},
                             {"name_conflict", desired_exists ? "true" : "false", true},
                             {"skip_existing", "false", true},
                             {"accepted", "true", true},
                         }));
}

int ServerApp::HandleScreenUploadChunk(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string transfer_id = util::Trim(JsonString(payload, "transfer_id"));
  if (transfer_id.empty()) {
    return Json(resp, 400, api::Error("transfer_id is required"));
  }
  const std::string data_base64 = JsonString(payload, "data_base64");
  if (data_base64.empty()) {
    return Json(resp, 400, api::Error("data_base64 is required"));
  }

  const std::string chunk = util::Base64Decode(data_base64);
  if (chunk.empty() && !data_base64.empty()) {
    // Allow empty chunk payloads that decode to empty bytes.
  }

  const std::string map_key = session->token + ":" + transfer_id;
  std::uint64_t received_bytes = 0;
  std::uint64_t expected_bytes = 0;
  std::filesystem::path temp_path;
  bool overrun = false;

  {
    std::lock_guard<std::mutex> lock(screen_upload_mu_);
    auto it = screen_uploads_.find(map_key);
    if (it == screen_uploads_.end()) {
      return Json(resp, 404, api::Error("transfer not found", "not_found"));
    }
    ScreenUploadTransfer& transfer = it->second;
    if (transfer.owner_session_token != session->token) {
      return Json(resp, 403, api::Error("forbidden", "forbidden"));
    }
    transfer.stream.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    if (!transfer.stream.good()) {
      temp_path = transfer.temp_path;
      transfer.stream.close();
      screen_uploads_.erase(it);
      std::error_code remove_error;
      std::filesystem::remove(temp_path, remove_error);
      return Json(resp, 500, api::Error("failed to append upload chunk"));
    }
    transfer.received_bytes += static_cast<std::uint64_t>(chunk.size());
    received_bytes = transfer.received_bytes;
    expected_bytes = transfer.expected_bytes;
    temp_path = transfer.temp_path;
    if (expected_bytes > 0 && received_bytes > expected_bytes) {
      transfer.stream.close();
      screen_uploads_.erase(it);
      overrun = true;
    }
  }

  if (overrun) {
    std::error_code remove_error;
    std::filesystem::remove(temp_path, remove_error);
    return Json(resp, 400, api::Error("received bytes exceed declared upload size"));
  }

  return Json(resp, 200, api::Success({
                             {"transfer_id", transfer_id, false},
                             {"received_bytes", std::to_string(received_bytes), true},
                             {"expected_bytes", std::to_string(expected_bytes), true},
                         }));
}

int ServerApp::HandleScreenUploadCommit(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string transfer_id = util::Trim(JsonString(payload, "transfer_id"));
  if (transfer_id.empty()) {
    return Json(resp, 400, api::Error("transfer_id is required"));
  }

  const std::string map_key = session->token + ":" + transfer_id;
  ScreenUploadTransfer transfer;
  {
    std::lock_guard<std::mutex> lock(screen_upload_mu_);
    auto it = screen_uploads_.find(map_key);
    if (it == screen_uploads_.end()) {
      return Json(resp, 404, api::Error("transfer not found", "not_found"));
    }
    transfer = std::move(it->second);
    screen_uploads_.erase(it);
  }

  transfer.stream.flush();
  transfer.stream.close();

  if (transfer.expected_bytes > 0 && transfer.received_bytes != transfer.expected_bytes) {
    std::error_code remove_error;
    std::filesystem::remove(transfer.temp_path, remove_error);
    return Json(resp, 400, api::Error("upload is incomplete"));
  }

  std::error_code fs_error;
  if (!std::filesystem::exists(transfer.target_directory, fs_error)) {
    std::filesystem::create_directories(transfer.target_directory, fs_error);
  }
  if (fs_error || !std::filesystem::is_directory(transfer.target_directory)) {
    std::filesystem::remove(transfer.temp_path, fs_error);
    return Json(resp, 500, api::Error("target directory is unavailable"));
  }

  std::string conflict_strategy = transfer.conflict_strategy;
  if (conflict_strategy.empty()) {
    conflict_strategy = "keep_both";
  }
  const std::string requested_safe_name = SanitizeUploadFilename(transfer.file_name);
  const std::filesystem::path desired_path = transfer.target_directory / requested_safe_name;
  const bool desired_exists = std::filesystem::exists(desired_path, fs_error);
  if (fs_error) {
    std::filesystem::remove(transfer.temp_path, fs_error);
    return Json(resp, 500, api::Error("failed to check target file state"));
  }
  if (desired_exists && std::filesystem::is_directory(desired_path, fs_error)) {
    std::filesystem::remove(transfer.temp_path, fs_error);
    return Json(resp, 400, api::Error("target path is a directory"));
  }

  bool skipped = false;
  bool overwritten = false;
  bool name_conflict = false;
  std::filesystem::path final_path;
  if (conflict_strategy == "overwrite") {
    final_path = desired_path;
    name_conflict = desired_exists;
    overwritten = desired_exists;
    if (desired_exists) {
      std::filesystem::remove(desired_path, fs_error);
      if (fs_error) {
        std::filesystem::remove(transfer.temp_path, fs_error);
        return Json(resp, 500, api::Error("failed to remove existing file for overwrite"));
      }
    }
  } else if (conflict_strategy == "skip" && desired_exists) {
    final_path = desired_path;
    skipped = true;
    name_conflict = true;
  } else {
    final_path = ResolveUniqueTargetPath(transfer.target_directory, transfer.file_name);
    name_conflict = desired_path != final_path;
  }

  if (skipped) {
    std::filesystem::remove(transfer.temp_path, fs_error);
    audit_logger_.Append(session->token, "screen.upload.skip", final_path.string());
    return Json(resp, 200, api::Success({
                               {"transfer_id", transfer_id, false},
                               {"name", requested_safe_name, false},
                               {"requested_name", requested_safe_name, false},
                               {"saved_name", requested_safe_name, false},
                               {"name_conflict", "true", true},
                               {"skipped", "true", true},
                               {"overwritten", "false", true},
                               {"path", final_path.string(), false},
                               {"target_dir", transfer.target_directory.string(), false},
                               {"bytes", "0", true},
                           }));
  }

  std::filesystem::rename(transfer.temp_path, final_path, fs_error);
  if (fs_error) {
    fs_error.clear();
    const auto copy_mode = conflict_strategy == "overwrite"
                               ? std::filesystem::copy_options::overwrite_existing
                               : std::filesystem::copy_options::none;
    std::filesystem::copy_file(transfer.temp_path, final_path, copy_mode, fs_error);
    if (fs_error) {
      std::filesystem::remove(transfer.temp_path, fs_error);
      return Json(resp, 500, api::Error("failed to finalize uploaded file"));
    }
    std::filesystem::remove(transfer.temp_path, fs_error);
  }

  audit_logger_.Append(session->token, "screen.upload", final_path.string());
  return Json(resp, 200, api::Success({
                             {"transfer_id", transfer_id, false},
                             {"name", SanitizeUploadFilename(transfer.file_name), false},
                             {"requested_name", requested_safe_name, false},
                             {"saved_name", final_path.filename().string(), false},
                             {"name_conflict", name_conflict ? "true" : "false", true},
                             {"skipped", "false", true},
                             {"overwritten", overwritten ? "true" : "false", true},
                             {"path", final_path.string(), false},
                             {"target_dir", transfer.target_directory.string(), false},
                             {"bytes", std::to_string(transfer.received_bytes), true},
                         }));
}

int ServerApp::HandleScreenUploadCancel(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string transfer_id = util::Trim(JsonString(payload, "transfer_id"));
  if (transfer_id.empty()) {
    return Json(resp, 400, api::Error("transfer_id is required"));
  }

  const std::string map_key = session->token + ":" + transfer_id;
  std::filesystem::path temp_path;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(screen_upload_mu_);
    auto it = screen_uploads_.find(map_key);
    if (it != screen_uploads_.end()) {
      found = true;
      temp_path = it->second.temp_path;
      it->second.stream.close();
      screen_uploads_.erase(it);
    }
  }
  if (found) {
    std::error_code remove_error;
    std::filesystem::remove(temp_path, remove_error);
  }

  return Json(resp, 200, api::Success({
                             {"transfer_id", transfer_id, false},
                             {"cancelled", found ? "true" : "false", true},
                         }));
}

int ServerApp::HandleTunnelState(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  std::string proxy_host;
  int proxy_port = 0;
  std::string proxy_token;
  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    proxy_host = config_.tunnel_proxy_host;
    proxy_port = config_.tunnel_proxy_port;
    proxy_token = config_.tunnel_proxy_token;
  }

  const auto snapshots = tunnel_manager_.Snapshot();
  std::vector<std::string> serialized;
  serialized.reserve(snapshots.size());
  for (const auto& snapshot : snapshots) {
    serialized.push_back(SerializeTunnelSnapshot(snapshot));
  }

  return Json(resp, 200, api::Success({
                             {"proxy_host", proxy_host, false},
                             {"proxy_port", std::to_string(proxy_port), true},
                             {"proxy_token", proxy_token, false},
                             {"mappings", JsonArray(serialized), true},
                         }));
}

int ServerApp::HandleTunnelConfigUpdate(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string requested_host = util::Trim(JsonString(payload, "proxy_host", QueryOf(req, "proxy_host")));
  const int requested_port = JsonInt(payload, "proxy_port", ParseInt(QueryOf(req, "proxy_port"), 0));
  const std::string token_query = QueryOf(req, "proxy_token");
  const bool has_requested_token =
      (payload.has_value() && payload->is_object() && payload->contains("proxy_token")) || !token_query.empty();
  const std::string requested_token = util::Trim(JsonString(payload, "proxy_token", token_query));

  std::string previous_host;
  int previous_port = 0;
  std::string previous_token;
  std::vector<core::TunnelMappingConfig> mappings;
  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    previous_host = config_.tunnel_proxy_host;
    previous_port = config_.tunnel_proxy_port;
    previous_token = config_.tunnel_proxy_token;
    mappings = config_.tunnel_mappings;
  }

  const std::string next_host = requested_host;
  const int next_port = requested_port > 0 ? requested_port : previous_port;
  const std::string next_token = has_requested_token ? requested_token : previous_token;
  if (!next_host.empty() && (next_port <= 0 || next_port > 65535)) {
    return Json(resp, 400, api::Error("proxy_port is invalid"));
  }

  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    config_.tunnel_proxy_host = next_host;
    config_.tunnel_proxy_port = next_port;
    config_.tunnel_proxy_token = next_token;
  }

  std::string persist_error;
  if (!PersistTunnelConfigToDisk(config_, next_host, next_port, next_token, mappings, &persist_error)) {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    config_.tunnel_proxy_host = previous_host;
    config_.tunnel_proxy_port = previous_port;
    config_.tunnel_proxy_token = previous_token;
    return Json(resp, 500, api::Error(persist_error.empty() ? "failed to persist tunnel config" : persist_error));
  }

  tunnel_manager_.Configure(next_host, next_port, next_token, mappings);
  audit_logger_.Append(session->token, "tunnel.config.update", next_host + ":" + std::to_string(next_port));

  return Json(resp, 200, api::Success({
                             {"proxy_host", next_host, false},
                             {"proxy_port", std::to_string(next_port), true},
                             {"proxy_token", next_token, false},
                         }));
}

int ServerApp::HandleTunnelMappingUpsert(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const auto parsed_mapping = ParseTunnelMappingFromJson(payload);
  if (!parsed_mapping.has_value()) {
    return Json(resp, 400, api::Error("invalid mapping payload"));
  }
  core::TunnelMappingConfig mapping = *parsed_mapping;

  std::vector<core::TunnelMappingConfig> previous_mappings;
  std::vector<core::TunnelMappingConfig> next_mappings;
  std::string proxy_host;
  int proxy_port = 0;
  std::string proxy_token;
  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    previous_mappings = config_.tunnel_mappings;
    next_mappings = config_.tunnel_mappings;
    proxy_host = config_.tunnel_proxy_host;
    proxy_port = config_.tunnel_proxy_port;
    proxy_token = config_.tunnel_proxy_token;
  }

  const std::string mapping_protocol = NormalizeTunnelProtocol(mapping.protocol);
  bool had_previous = false;
  core::TunnelMappingConfig previous_mapping;
  for (const auto& existing : previous_mappings) {
    if (existing.id == mapping.id) {
      previous_mapping = existing;
      had_previous = true;
      break;
    }
  }

  bool replaced = false;
  for (auto& existing : next_mappings) {
    if (existing.id == mapping.id) {
      existing = mapping;
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    next_mappings.push_back(mapping);
  }

  if (mapping.enabled) {
    for (const auto& item : next_mappings) {
      if (item.id == mapping.id || !item.enabled) {
        continue;
      }
      if (NormalizeTunnelProtocol(item.protocol) == mapping_protocol && item.remote_port == mapping.remote_port) {
        return Json(resp, 409, api::Error("remote port conflict with another mapping", "conflict"));
      }
    }
  }

  const bool keeps_existing_bound_remote_port =
      had_previous && previous_mapping.enabled && mapping.enabled &&
      NormalizeTunnelProtocol(previous_mapping.protocol) == mapping_protocol &&
      previous_mapping.remote_port == mapping.remote_port;
  if (mapping.enabled && !keeps_existing_bound_remote_port && TunnelProxyHostLooksLocal(proxy_host)) {
    const auto listening_ports = tunnel::PortInspector::ListListeningPorts();
    const auto occupied = FindListeningPortConflict(listening_ports, mapping_protocol, mapping.remote_port);
    if (occupied.has_value()) {
      std::ostringstream message;
      message << "remote (public) port " << mapping.remote_port << "/" << mapping_protocol << " is already occupied";
      const std::string address = util::Trim(occupied->address).empty() ? "0.0.0.0" : occupied->address;
      message << " at " << address;
      if (occupied->pid > 0 || !occupied->process.empty()) {
        message << " by ";
        if (occupied->pid > 0) {
          message << "pid " << occupied->pid;
          if (!occupied->process.empty()) {
            message << ' ';
          }
        }
        if (!occupied->process.empty()) {
          message << occupied->process;
        }
      }
      return Json(resp, 409, api::Error(message.str(), "port_occupied"));
    }
  }

  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    config_.tunnel_mappings = next_mappings;
  }

  std::string persist_error;
  if (!PersistTunnelConfigToDisk(config_, proxy_host, proxy_port, proxy_token, next_mappings, &persist_error)) {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    config_.tunnel_mappings = previous_mappings;
    return Json(resp, 500, api::Error(persist_error.empty() ? "failed to persist mapping" : persist_error));
  }

  tunnel_manager_.Configure(proxy_host, proxy_port, proxy_token, next_mappings);
  audit_logger_.Append(session->token, "tunnel.mapping.upsert",
                       mapping.id + " " + NormalizeTunnelProtocol(mapping.protocol) + " " +
                           std::to_string(mapping.local_port) + "->" + std::to_string(mapping.remote_port));

  return Json(resp, 200, api::Success({
                             {"mapping", SerializeTunnelMappingConfig(mapping), true},
                             {"updated", replaced ? "true" : "false", true},
                         }));
}

int ServerApp::HandleTunnelMappingDelete(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string mapping_id = util::Trim(JsonString(payload, "id", QueryOf(req, "id")));
  if (mapping_id.empty()) {
    return Json(resp, 400, api::Error("id is required"));
  }

  std::vector<core::TunnelMappingConfig> previous_mappings;
  std::vector<core::TunnelMappingConfig> next_mappings;
  std::string proxy_host;
  int proxy_port = 0;
  std::string proxy_token;
  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    previous_mappings = config_.tunnel_mappings;
    next_mappings = config_.tunnel_mappings;
    proxy_host = config_.tunnel_proxy_host;
    proxy_port = config_.tunnel_proxy_port;
    proxy_token = config_.tunnel_proxy_token;
  }

  const auto before_count = next_mappings.size();
  next_mappings.erase(std::remove_if(next_mappings.begin(), next_mappings.end(), [&](const core::TunnelMappingConfig& item) {
                      return item.id == mapping_id;
                    }),
                    next_mappings.end());
  if (next_mappings.size() == before_count) {
    return Json(resp, 404, api::Error("mapping not found", "not_found"));
  }

  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    config_.tunnel_mappings = next_mappings;
  }

  std::string persist_error;
  if (!PersistTunnelConfigToDisk(config_, proxy_host, proxy_port, proxy_token, next_mappings, &persist_error)) {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    config_.tunnel_mappings = previous_mappings;
    return Json(resp, 500, api::Error(persist_error.empty() ? "failed to persist mapping" : persist_error));
  }

  tunnel_manager_.Configure(proxy_host, proxy_port, proxy_token, next_mappings);
  audit_logger_.Append(session->token, "tunnel.mapping.delete", mapping_id);

  return Json(resp, 200, api::Success({
                             {"id", mapping_id, false},
                         }));
}

int ServerApp::HandleTunnelMappingTest(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string mapping_id = util::Trim(JsonString(payload, "id", QueryOf(req, "id")));
  if (mapping_id.empty()) {
    return Json(resp, 400, api::Error("id is required"));
  }

  bool exists = false;
  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    for (const auto& item : config_.tunnel_mappings) {
      if (item.id == mapping_id) {
        exists = true;
        break;
      }
    }
  }
  if (!exists) {
    return Json(resp, 404, api::Error("mapping not found", "not_found"));
  }

  bool test_ok = false;
  std::string detail;
  const bool executed = tunnel_manager_.TestMapping(mapping_id, &test_ok, &detail, 5000);
  if (!executed && detail.empty()) {
    return Json(resp, 500, api::Error("failed to execute mapping test"));
  }

  audit_logger_.Append(session->token, "tunnel.mapping.test",
                       mapping_id + " result=" + std::string(test_ok ? "ok" : "failed") + " detail=" + detail);

  return Json(resp, 200, api::Success({
                             {"id", mapping_id, false},
                             {"test_ok", test_ok ? "true" : "false", true},
                             {"detail", detail, false},
                         }));
}

int ServerApp::HandleTunnelPorts(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  static std::mutex ports_cache_mu;
  static std::vector<tunnel::ListeningPortInfo> ports_cache;
  static auto ports_cache_at = std::chrono::steady_clock::time_point{};
  constexpr auto kPortsCacheTtl = std::chrono::milliseconds(1800);

  std::vector<tunnel::ListeningPortInfo> ports;
  bool cache_hit = false;
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> cache_lock(ports_cache_mu);
    if (ports_cache_at != std::chrono::steady_clock::time_point{} && (now - ports_cache_at) < kPortsCacheTtl) {
      ports = ports_cache;
      cache_hit = true;
    }
  }
  if (!cache_hit) {
    ports = tunnel::PortInspector::ListListeningPorts();
    std::lock_guard<std::mutex> cache_lock(ports_cache_mu);
    ports_cache = ports;
    ports_cache_at = std::chrono::steady_clock::now();
  }

  std::vector<std::string> serialized;
  serialized.reserve(ports.size());
  for (const auto& item : ports) {
    serialized.push_back(util::BuildJsonObject({
        {"protocol", item.protocol, false},
        {"address", item.address, false},
        {"port", std::to_string(item.port), true},
        {"process", item.process, false},
        {"pid", std::to_string(item.pid), true},
    }));
  }

  return Json(resp, 200, api::Success({
                             {"items", JsonArray(serialized), true},
                             {"count", std::to_string(serialized.size()), true},
                         }));
}

int ServerApp::HandleHealth(HttpRequest* req, HttpResponse* resp) {
  (void)req;
  return Json(resp, 200, api::Success({
                             {"service", "ferryman", false},
                             {"running", running_ ? "true" : "false", true},
                             {"http_port", std::to_string(config_.http_port), true},
                             {"https_enabled", config_.https_enabled ? "true" : "false", true},
                             {"https_port", std::to_string(config_.https_port), true},
                             {"ws_port", std::to_string(config_.ws_port), true},
                         }));
}

int ServerApp::HandleStaticAsset(HttpRequest* req, HttpResponse* resp) {
  if (req->path.rfind("/api/", 0) == 0) {
    return Json(resp, 404, api::Error("not found", "not_found"));
  }

  auto asset = FindEmbeddedAsset(req->path);
  if (!asset.has_value()) {
    return Text(resp, 404, "Not Found");
  }
  return Text(resp, 200, std::string(asset->content), std::string(asset->mime_type));
}

void ServerApp::SendToWs(std::uintptr_t channel_key, const std::string& payload) {
  WebSocketChannelPtr channel;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    auto it = ws_clients_.find(channel_key);
    if (it == ws_clients_.end()) {
      return;
    }
    channel = it->second.channel;
  }
  if (channel) {
    channel->send(payload);
  }
}

ServerApp::NativeCaptureDemand ServerApp::CollectNativeCaptureDemandLocked() const {
  NativeCaptureDemand demand;
  std::unordered_map<std::string, size_t> source_counts;
  for (const auto& [_, client] : ws_clients_) {
    if (client.channel_type != "webrtc" || !client.native_stream_subscribed || !client.channel) {
      continue;
    }
    demand.subscriber_count += 1;
    demand.fps = std::max(demand.fps, std::clamp(client.native_stream_fps, kNativeCaptureMinFps,
                                                 kNativeCaptureMaxFps));
    if (client.native_stream_codec == "av1") {
      demand.want_av1 = true;
    } else if (client.native_stream_codec == "vp9") {
      demand.want_vp9 = true;
    } else if (client.native_stream_codec == "vp8") {
      demand.want_vp8 = true;
    } else if (client.native_stream_codec == "h265") {
      demand.want_h265 = true;
    } else if (client.native_stream_codec == "h264") {
      demand.want_h264 = true;
    } else {
      demand.want_jpeg = true;
    }
    demand.scale_percent =
        std::max(demand.scale_percent, std::clamp(client.native_stream_scale_percent, kNativeScaleMinPercent,
                                                  kNativeScaleMaxPercent));
    demand.video_bitrate_bps =
        std::max(demand.video_bitrate_bps,
                 std::clamp(client.native_stream_video_bitrate_bps, kNativeBitrateMinBps, kNativeBitrateMaxBps));
    source_counts[client.native_stream_source_id] += 1;
  }

  size_t best_source_votes = 0;
  for (const auto& [source_id, votes] : source_counts) {
    if (votes > best_source_votes || (votes == best_source_votes && source_id < demand.source_id)) {
      demand.source_id = source_id;
      best_source_votes = votes;
    }
  }
  return demand;
}

void ServerApp::RefreshNativeCaptureState(const std::string& actor_session_token) {
  const auto append_capture_log = [this, &actor_session_token](const std::string& level,
                                                                const std::string& detail) {
    if (actor_session_token.empty()) {
      audit_logger_.AppendSystem(level, "screen.capture", detail);
      return;
    }
    audit_logger_.AppendWithLevel(level, actor_session_token, "screen.capture", detail);
  };

  NativeCaptureDemand demand;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    demand = CollectNativeCaptureDemandLocked();
  }

  screen_service_.SetEncodingTargets(demand.want_jpeg, demand.want_h264, demand.want_h265, demand.want_vp8,
                                     demand.want_vp9, demand.want_av1);

  if (demand.subscriber_count == 0) {
    if (screen_service_.IsCapturing()) {
      screen_service_.StopCapture();
      append_capture_log("info", "native capture stopped (no subscribers)");
    }
    active_capture_fps_ = 0;
    active_capture_scale_percent_ = kNativeScaleDefaultPercent;
    active_capture_video_bitrate_bps_ = kNativeBitrateDefaultBps;
    return;
  }

  const int target_fps = std::clamp(demand.fps > 0 ? demand.fps : kNativeCaptureFps, kNativeCaptureMinFps,
                                    kNativeCaptureMaxFps);
  const int target_scale_percent =
      std::clamp(demand.scale_percent > 0 ? demand.scale_percent : kNativeScaleDefaultPercent,
                 kNativeScaleMinPercent, kNativeScaleMaxPercent);
  const int target_video_bitrate_bps =
      std::clamp(demand.video_bitrate_bps > 0 ? demand.video_bitrate_bps : kNativeBitrateDefaultBps,
                 kNativeBitrateMinBps, kNativeBitrateMaxBps);
  std::string source_error;
  const std::string target_source_id = screen_service_.NormalizeCaptureSourceId(demand.source_id, &source_error);
  if (target_source_id.empty()) {
    if (screen_service_.IsCapturing()) {
      screen_service_.StopCapture();
    }
    active_capture_fps_ = 0;
    append_capture_log("warn", source_error.empty() ? "no capture source available" : source_error);
    return;
  }

  screen_service_.SetEncodingProfile(target_scale_percent, target_video_bitrate_bps);
  active_capture_scale_percent_ = target_scale_percent;
  active_capture_video_bitrate_bps_ = target_video_bitrate_bps;

  const std::string active_source_id = screen_service_.ActiveCaptureSourceId();
  if (screen_service_.IsCapturing() && active_capture_fps_ == target_fps && active_source_id == target_source_id) {
    return;
  }

  if (screen_service_.IsCapturing()) {
    screen_service_.StopCapture();
    active_capture_fps_ = 0;
  }

  std::string screen_error;
  if (!screen_service_.StartCapture(target_fps, target_source_id, &screen_error)) {
    std::cerr << "[ferryman] native screen capture unavailable: " << screen_error << '\n';
    append_capture_log("warn", screen_error);
    return;
  }
  active_capture_fps_ = target_fps;
  const std::string running_source_id = screen_service_.ActiveCaptureSourceId();

  append_capture_log("info",
                     "native capture started (subscribers=" +
                         std::to_string(demand.subscriber_count) +
                         ", fps=" + std::to_string(target_fps) +
                         ", scale=" + std::to_string(target_scale_percent) +
                         ", bitrate=" + std::to_string(target_video_bitrate_bps) +
                         ", source_id=" + (running_source_id.empty() ? target_source_id : running_source_id) + ")");
}

void ServerApp::SyncNativeSubscribersToActiveSource() {
  const std::string active_source_id = screen_service_.ActiveCaptureSourceId();
  if (active_source_id.empty()) {
    return;
  }

  const int effective_fps = active_capture_fps_.load();
  const int effective_scale_percent = active_capture_scale_percent_.load();
  const int effective_video_bitrate_bps = active_capture_video_bitrate_bps_.load();

  struct UpdateTarget {
    WebSocketChannelPtr channel;
    std::string codec;
  };
  std::vector<UpdateTarget> targets;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    for (auto& [_, client] : ws_clients_) {
      if (client.channel_type != "webrtc" || !client.native_stream_subscribed || !client.channel) {
        continue;
      }
      if (client.native_stream_source_id == active_source_id) {
        continue;
      }
      client.native_stream_source_id = active_source_id;
      targets.push_back({client.channel, client.native_stream_codec});
    }
  }
  if (targets.empty()) {
    return;
  }

  const std::string resolution_tier = NativeResolutionTierFromScale(effective_scale_percent);
  const std::string bitrate_tier = NativeBitrateTierFromBps(effective_video_bitrate_bps);
  for (auto& target : targets) {
    target.channel->send(api::Success({
        {"event", "native_subscribed", false},
        {"capture_running", screen_service_.IsCapturing() ? "true" : "false", true},
        {"codec", target.codec, false},
        {"source_id", active_source_id, false},
        {"fps", std::to_string(effective_fps > 0 ? effective_fps : kNativeCaptureFps), true},
        {"resolution_tier", resolution_tier, false},
        {"scale_percent", std::to_string(effective_scale_percent), true},
        {"bitrate_tier", bitrate_tier, false},
        {"bitrate_bps", std::to_string(effective_video_bitrate_bps), true},
        {"transport", "ws-binary", false},
    }));
  }
}

void ServerApp::HandleWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
  const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(channel.get());
  std::string token;
  auto token_it = req->query_params.find("token");
  if (token_it != req->query_params.end()) {
    token = token_it->second;
  }

  auto session = session_manager_.GetSession(token);
  if (!session.has_value()) {
    audit_logger_.AppendSystem("warn", "ws.open.reject", "unauthorized token on " + req->path);
    channel->send(api::Error("unauthorized", "unauthorized"));
    channel->close();
    return;
  }

  std::string ws_path = req->path;
  const size_t qpos = ws_path.find('?');
  if (qpos != std::string::npos) {
    ws_path = ws_path.substr(0, qpos);
  }

  std::string channel_type;
  if (ws_path == "/ws/terminal") {
    channel_type = "terminal";
  } else if (ws_path == "/ws/webrtc") {
    channel_type = "webrtc";
  } else if (ws_path == "/ws/logs") {
    channel_type = "logs";
  } else if (ws_path == "/ws/dockurr") {
    channel_type = "dockurr";
  } else if (ws_path == "/ws/monitor") {
    channel_type = "monitor";
  } else {
    audit_logger_.Append(session->token, "ws.open.reject", "unknown path: " + req->path);
    channel->send(api::Error("unknown websocket path"));
    channel->close();
    return;
  }

  WsClient client;
  client.channel_type = channel_type;
  client.session_token = token;
  client.channel = channel;

  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    ws_clients_[key] = client;
  }

  audit_logger_.Append(token, "ws.open", channel_type);
  channel->send(api::Success({
      {"event", "connected", false},
      {"channel", channel_type, false},
  }));

  if (channel_type == "logs") {
    const std::string snapshot = api::Success({
        {"event", "logs_snapshot", false},
        {"items", audit_logger_.Tail(300), true},
    });
    channel->send(snapshot);
  } else if (channel_type == "dockurr") {
    std::string error;
    const auto vms = dockurr_manager_.ListVms(&error);
    if (!error.empty()) {
      channel->send(api::Success({
          {"event", "dockurr_error", false},
          {"error", error, false},
      }));
    } else {
      channel->send(BuildDockurrSnapshotPayload(vms));
    }
  } else if (channel_type == "monitor") {
    channel->send(api::Success({
        {"event", "monitor_snapshot", false},
        {"snapshot", system_monitor_.SnapshotJson(), true},
    }));
  }
}

void ServerApp::HandleWsMessage(const WebSocketChannelPtr& channel, const std::string& message) {
  const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(channel.get());
  std::string channel_type;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    auto it = ws_clients_.find(key);
    if (it == ws_clients_.end()) {
      return;
    }
    channel_type = it->second.channel_type;
  }

  if (channel_type == "terminal") {
    HandleTerminalWsMessage(key, message);
  } else if (channel_type == "webrtc") {
    HandleWebRtcWsMessage(key, message);
  } else if (channel_type == "logs") {
    HandleLogsWsMessage(key, message);
  } else if (channel_type == "dockurr") {
    HandleDockurrWsMessage(key, message);
  } else if (channel_type == "monitor") {
    HandleMonitorWsMessage(key, message);
  } else {
    audit_logger_.AppendSystem("warn", "ws.message.reject", "unknown channel type");
  }
}

void ServerApp::HandleWsClose(const WebSocketChannelPtr& channel) {
  const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(channel.get());
  WsClient client;
  bool found = false;
  bool should_refresh_capture = false;

  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    auto it = ws_clients_.find(key);
    if (it != ws_clients_.end()) {
      client = it->second;
      should_refresh_capture = client.channel_type == "webrtc" && client.native_stream_subscribed;
      ws_clients_.erase(it);
      found = true;
    }
  }

  if (!found) {
    return;
  }

  if (client.channel_type == "terminal" && !client.terminal_id.empty()) {
    std::string error;
    pty_manager_.CloseTerminal(client.session_token, client.terminal_id, &error);
  }

  if (client.channel_type == "webrtc") {
    signaling_service_.Leave(key);
  }

  if (should_refresh_capture) {
    RefreshNativeCaptureState(client.session_token);
  }

  audit_logger_.Append(client.session_token, "ws.close", client.channel_type);
}

void ServerApp::HandleTerminalWsMessage(std::uintptr_t channel_key, const std::string& message) {
  const auto payload = ParseJsonOrNull(message);
  if (!payload.has_value() || !payload->is_object()) {
    audit_logger_.AppendSystem("warn", "terminal.ws.invalid", "invalid json payload");
    SendToWs(channel_key, api::Error("invalid json payload"));
    return;
  }
  const std::string action = JsonString(payload, "action");

  WsClient client;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    auto it = ws_clients_.find(channel_key);
    if (it == ws_clients_.end()) {
      return;
    }
    client = it->second;
  }

  auto session = session_manager_.GetSession(client.session_token);
  if (!session.has_value()) {
    audit_logger_.Append(client.session_token, "terminal.ws.reject", "session expired");
    SendToWs(channel_key, api::Error("session expired", "unauthorized"));
    return;
  }

  if (action == "open") {
    audit_logger_.Append(client.session_token, "terminal.open.request",
                         "cols=" + std::to_string(JsonInt(payload, "cols", 120)) +
                             ", rows=" + std::to_string(JsonInt(payload, "rows", 30)));

    std::string error;
    auto terminal_id = pty_manager_.CreateTerminal(client.session_token,
                                                   JsonInt(payload, "cols", 120),
                                                   JsonInt(payload, "rows", 30),
                                                   &error);
    if (!terminal_id.has_value()) {
      audit_logger_.Append(client.session_token, "terminal.open.error",
                           error.empty() ? "failed to create terminal" : error);
      SendToWs(channel_key, api::Error(error.empty() ? "failed to create terminal" : error));
      return;
    }

    {
      std::lock_guard<std::mutex> lock(ws_mu_);
      auto it = ws_clients_.find(channel_key);
      if (it != ws_clients_.end()) {
        it->second.terminal_id = *terminal_id;
      }
    }

    audit_logger_.Append(client.session_token, "terminal.open", *terminal_id);
    SendToWs(channel_key, api::Success({
        {"event", "terminal_open", false},
        {"terminal_id", *terminal_id, false},
    }));
    return;
  }

  if (action == "attach") {
    const std::string terminal_id = JsonString(payload, "terminal_id");
    const auto terminals = pty_manager_.ListTerminals(client.session_token);
    if (std::find(terminals.begin(), terminals.end(), terminal_id) == terminals.end()) {
      audit_logger_.Append(client.session_token, "terminal.attach.error", "terminal not found: " + terminal_id);
      SendToWs(channel_key, api::Error("terminal not found", "not_found"));
      return;
    }

    {
      std::lock_guard<std::mutex> lock(ws_mu_);
      auto it = ws_clients_.find(channel_key);
      if (it != ws_clients_.end()) {
        it->second.terminal_id = terminal_id;
      }
    }

    SendToWs(channel_key, api::Success({
        {"event", "terminal_attached", false},
        {"terminal_id", terminal_id, false},
    }));
    return;
  }

  std::string terminal_id = client.terminal_id;
  if (payload->contains("terminal_id")) {
    terminal_id = JsonString(payload, "terminal_id");
  }
  if (terminal_id.empty()) {
    audit_logger_.Append(client.session_token, "terminal.ws.reject", "terminal is not attached");
    SendToWs(channel_key, api::Error("terminal is not attached"));
    return;
  }

  if (action == "input") {
    const std::string encoded = JsonString(payload, "data");
    const std::string data = util::Base64Decode(encoded);
    std::string error;
    if (!pty_manager_.WriteInput(client.session_token, terminal_id, data, &error)) {
      audit_logger_.Append(client.session_token, "terminal.input.error",
                           error.empty() ? "write failed" : error);
      SendToWs(channel_key, api::Error(error.empty() ? "write failed" : error));
      return;
    }
    return;
  }

  if (action == "resize") {
    std::string error;
    if (!pty_manager_.Resize(client.session_token, terminal_id,
                             JsonInt(payload, "cols", 120),
                             JsonInt(payload, "rows", 30),
                             &error)) {
      audit_logger_.Append(client.session_token, "terminal.resize.error",
                           error.empty() ? "resize failed" : error);
      SendToWs(channel_key, api::Error(error.empty() ? "resize failed" : error));
      return;
    }
    return;
  }

  if (action == "close") {
    std::string error;
    if (!pty_manager_.CloseTerminal(client.session_token, terminal_id, &error)) {
      audit_logger_.Append(client.session_token, "terminal.close.error",
                           error.empty() ? "close failed" : error);
      SendToWs(channel_key, api::Error(error.empty() ? "close failed" : error));
      return;
    }
    {
      std::lock_guard<std::mutex> lock(ws_mu_);
      auto it = ws_clients_.find(channel_key);
      if (it != ws_clients_.end()) {
        it->second.terminal_id.clear();
      }
    }
    SendToWs(channel_key, api::Success({
        {"event", "terminal_closed", false},
        {"terminal_id", terminal_id, false},
    }));
    return;
  }

  audit_logger_.Append(client.session_token, "terminal.ws.invalid_action", action);
  SendToWs(channel_key, api::Error("unknown terminal action"));
}

void ServerApp::BroadcastTerminalOutput(const std::string& terminal_id, const std::string& chunk) {
  std::vector<WebSocketChannelPtr> channels;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    for (const auto& [_, client] : ws_clients_) {
      if (client.channel_type == "terminal" && client.terminal_id == terminal_id && client.channel) {
        channels.push_back(client.channel);
      }
    }
  }

  if (channels.empty()) {
    return;
  }

  const std::string payload = api::Success({
      {"event", "terminal_output", false},
      {"terminal_id", terminal_id, false},
      {"data", util::Base64Encode(chunk), false},
  });

  for (auto& channel : channels) {
    channel->send(payload);
  }
}

void ServerApp::BroadcastLogEntry(const std::string& serialized_entry) {
  std::vector<WebSocketChannelPtr> channels;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    for (const auto& [_, client] : ws_clients_) {
      if (client.channel_type == "logs" && client.channel) {
        channels.push_back(client.channel);
      }
    }
  }

  if (channels.empty()) {
    return;
  }

  const std::string payload = api::Success({
      {"event", "log_entry", false},
      {"item", serialized_entry, true},
  });
  for (auto& channel : channels) {
    channel->send(payload);
  }
}

void ServerApp::BroadcastNativeFrames() {
  uint64_t last_sequence = 0;
  auto next_source_health_check = std::chrono::steady_clock::now();
  while (running_) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_source_health_check) {
      RefreshNativeCaptureState();
      SyncNativeSubscribersToActiveSource();
      next_source_health_check = now + std::chrono::seconds(1);
    }

    auto frame = screen_service_.LatestFrame();
    if (!frame.has_value() || frame->sequence == last_sequence) {
      std::this_thread::sleep_for(std::chrono::milliseconds(24));
      continue;
    }
    last_sequence = frame->sequence;

    struct Target {
      WebSocketChannelPtr channel;
      std::string codec;
    };
    std::vector<Target> targets;
    {
      std::lock_guard<std::mutex> lock(ws_mu_);
      for (const auto& [_, client] : ws_clients_) {
        if (client.channel_type == "webrtc" && client.native_stream_subscribed && client.channel) {
          targets.push_back({client.channel, client.native_stream_codec});
        }
      }
    }
    if (targets.empty()) {
      continue;
    }

    const bool has_h264 = !frame->h264_bytes.empty();
    const bool has_h265 = !frame->h265_bytes.empty();
    const bool has_vp8 = !frame->vp8_bytes.empty();
    const bool has_vp9 = !frame->vp9_bytes.empty();
    const bool has_av1 = !frame->av1_bytes.empty();
    const bool has_jpeg = !frame->jpeg_bytes.empty();
    if (!has_h264 && !has_h265 && !has_vp8 && !has_vp9 && !has_av1 && !has_jpeg) {
      continue;
    }

    const std::string h264_payload = has_h264
        ? BuildNativeBinaryFramePacket(kNativeBinaryCodecH264, frame->h264_keyframe, frame->sequence,
                                       frame->captured_at_ms, frame->width, frame->height, frame->h264_bytes)
        : "";
    const std::string h265_payload = has_h265
        ? BuildNativeBinaryFramePacket(kNativeBinaryCodecH265, frame->h265_keyframe, frame->sequence,
                                       frame->captured_at_ms, frame->width, frame->height, frame->h265_bytes)
        : "";
    const std::string vp8_payload = has_vp8
        ? BuildNativeBinaryFramePacket(kNativeBinaryCodecVP8, frame->vp8_keyframe, frame->sequence,
                                       frame->captured_at_ms, frame->width, frame->height, frame->vp8_bytes)
        : "";
    const std::string vp9_payload = has_vp9
        ? BuildNativeBinaryFramePacket(kNativeBinaryCodecVP9, frame->vp9_keyframe, frame->sequence,
                                       frame->captured_at_ms, frame->width, frame->height, frame->vp9_bytes)
        : "";
    const std::string av1_payload = has_av1
        ? BuildNativeBinaryFramePacket(kNativeBinaryCodecAV1, frame->av1_keyframe, frame->sequence,
                                       frame->captured_at_ms, frame->width, frame->height, frame->av1_bytes)
        : "";
    const std::string jpeg_payload = has_jpeg
        ? BuildNativeBinaryFramePacket(kNativeBinaryCodecJpeg, false, frame->sequence,
                                       frame->captured_at_ms, frame->width, frame->height, frame->jpeg_bytes)
        : "";

    for (auto& target : targets) {
      if (target.codec == "av1" && !av1_payload.empty()) {
        target.channel->send(av1_payload, WS_OPCODE_BINARY);
        continue;
      }
      if (target.codec == "vp9" && !vp9_payload.empty()) {
        target.channel->send(vp9_payload, WS_OPCODE_BINARY);
        continue;
      }
      if (target.codec == "vp8" && !vp8_payload.empty()) {
        target.channel->send(vp8_payload, WS_OPCODE_BINARY);
        continue;
      }
      if (target.codec == "h265" && !h265_payload.empty()) {
        target.channel->send(h265_payload, WS_OPCODE_BINARY);
        continue;
      }
      if (target.codec == "h264" && !h264_payload.empty()) {
        target.channel->send(h264_payload, WS_OPCODE_BINARY);
        continue;
      }
      if (!jpeg_payload.empty()) {
        target.channel->send(jpeg_payload, WS_OPCODE_BINARY);
      }
    }
  }
}

void ServerApp::BroadcastDockurrSnapshots() {
  std::string last_snapshot_payload;
  std::string last_error;
  while (running_) {
    std::vector<std::uintptr_t> channels;
    {
      std::lock_guard<std::mutex> lock(ws_mu_);
      channels.reserve(ws_clients_.size());
      for (const auto& [channel_key, client] : ws_clients_) {
        if (client.channel_type == "dockurr" && client.channel) {
          channels.push_back(channel_key);
        }
      }
    }

    if (channels.empty()) {
      last_snapshot_payload.clear();
      last_error.clear();
      std::this_thread::sleep_for(std::chrono::milliseconds(kDockurrSnapshotIntervalMs));
      continue;
    }

    std::string list_error;
    const auto vms = dockurr_manager_.ListVms(&list_error);
    if (!list_error.empty()) {
      if (list_error != last_error) {
        const std::string payload = api::Success({
            {"event", "dockurr_error", false},
            {"error", list_error, false},
        });
        const std::string log_payload =
            BuildDockurrRuntimeLogPayload("error", "list", "snapshot failed: " + list_error);
        for (const auto& channel_key : channels) {
          SendToWs(channel_key, payload);
          SendToWs(channel_key, log_payload);
        }
        last_error = list_error;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kDockurrSnapshotIntervalMs));
      continue;
    }

    const std::string payload = BuildDockurrSnapshotPayload(vms);
    if (payload != last_snapshot_payload) {
      for (const auto& channel_key : channels) {
        SendToWs(channel_key, payload);
      }
      last_snapshot_payload = payload;
    }
    last_error.clear();

    std::this_thread::sleep_for(std::chrono::milliseconds(kDockurrSnapshotIntervalMs));
  }
}

void ServerApp::BroadcastMonitorSnapshots() {
  while (running_) {
    std::vector<std::uintptr_t> channels;
    {
      std::lock_guard<std::mutex> lock(ws_mu_);
      channels.reserve(ws_clients_.size());
      for (const auto& [channel_key, client] : ws_clients_) {
        if (client.channel_type == "monitor" && client.channel) {
          channels.push_back(channel_key);
        }
      }
    }

    if (channels.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kMonitorSnapshotIntervalMs));
      continue;
    }

    const std::string payload = api::Success({
        {"event", "monitor_snapshot", false},
        {"snapshot", system_monitor_.SnapshotJson(), true},
    });
    for (const auto& channel_key : channels) {
      SendToWs(channel_key, payload);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kMonitorSnapshotIntervalMs));
  }
}

void ServerApp::HandleWebRtcWsMessage(std::uintptr_t channel_key, const std::string& message) {
  const auto payload = ParseJsonOrNull(message);
  if (!payload.has_value() || !payload->is_object()) {
    audit_logger_.AppendSystem("warn", "webrtc.ws.invalid", "invalid json payload");
    SendToWs(channel_key, api::Error("invalid json payload"));
    return;
  }
  const std::string action = JsonString(payload, "action");

  WsClient client;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    auto it = ws_clients_.find(channel_key);
    if (it == ws_clients_.end()) {
      return;
    }
    client = it->second;
  }

  auto session = session_manager_.GetSession(client.session_token);
  if (!session.has_value()) {
    audit_logger_.Append(client.session_token, "webrtc.ws.reject", "session expired");
    SendToWs(channel_key, api::Error("session expired", "unauthorized"));
    return;
  }
  (void)session;

  if (action == "join") {
    const std::string room_id = JsonString(payload, "room_id");
    auto peer = signaling_service_.JoinRoom(channel_key, client.session_token, room_id);
    if (!peer.has_value()) {
      SendToWs(channel_key, api::Error("room_id is required"));
      return;
    }

    {
      std::lock_guard<std::mutex> lock(ws_mu_);
      auto it = ws_clients_.find(channel_key);
      if (it != ws_clients_.end()) {
        it->second.room_id = peer->room_id;
        it->second.peer_id = peer->peer_id;
      }
    }

    const auto peers = signaling_service_.PeersInRoom(peer->room_id);
    std::vector<std::string> peer_ids;
    std::vector<std::uintptr_t> notify_channels;
    for (const auto& item : peers) {
      if (item.peer_id != peer->peer_id) {
        peer_ids.push_back('"' + util::JsonEscape(item.peer_id) + '"');
        auto target = signaling_service_.FindChannelByPeerId(item.peer_id);
        if (target.has_value()) {
          notify_channels.push_back(*target);
        }
      }
    }

    SendToWs(channel_key, api::Success({
        {"event", "joined", false},
        {"room_id", peer->room_id, false},
        {"peer_id", peer->peer_id, false},
        {"peers", JsonArray(peer_ids), true},
    }));

    const std::string join_notice = api::Success({
        {"event", "peer_join", false},
        {"peer_id", peer->peer_id, false},
    });
    for (const auto& target_channel : notify_channels) {
      SendToWs(target_channel, join_notice);
    }

    audit_logger_.Append(client.session_token, "webrtc.join", room_id);
    return;
  }

  if (action == "signal") {
    const auto sender = signaling_service_.GetPeer(channel_key);
    if (!sender.has_value()) {
      SendToWs(channel_key, api::Error("join room first"));
      return;
    }

    const std::string signal_type = JsonString(payload, "signal_type");
    std::string signal_payload = JsonString(payload, "payload");
    if (payload->contains("payload") && !(*payload)["payload"].is_string()) {
      signal_payload = (*payload)["payload"].dump();
    }
    const std::string target_peer_id = JsonString(payload, "target_peer_id");

    std::vector<std::uintptr_t> targets;
    if (!target_peer_id.empty()) {
      auto target = signaling_service_.FindChannelByPeerId(target_peer_id);
      if (target.has_value()) {
        targets.push_back(*target);
      }
    } else {
      for (const auto& peer : signaling_service_.PeersInRoom(sender->room_id)) {
        if (peer.peer_id == sender->peer_id) {
          continue;
        }
        auto target = signaling_service_.FindChannelByPeerId(peer.peer_id);
        if (target.has_value()) {
          targets.push_back(*target);
        }
      }
    }

    const std::string forwarded = api::Success({
        {"event", "signal", false},
        {"from_peer_id", sender->peer_id, false},
        {"signal_type", signal_type, false},
        {"payload", signal_payload, false},
    });
    for (const auto& target : targets) {
      SendToWs(target, forwarded);
    }
    return;
  }

  if (action == "native_subscribe") {
    const std::string requested_codec = JsonString(payload, "codec");
    const std::string requested_source_id = JsonString(payload, "source_id");
    const bool wants_av1 = requested_codec == "av1";
    const bool wants_vp9 = requested_codec == "vp9";
    const bool wants_vp8 = requested_codec == "vp8";
    const bool wants_h265 = requested_codec == "h265";
    const bool wants_h264 = requested_codec == "h264";
    std::string negotiated_codec = "jpeg";
    if (wants_av1 && screen_service_.SupportsAV1()) {
      negotiated_codec = "av1";
    } else if (wants_vp9 && screen_service_.SupportsVP9()) {
      negotiated_codec = "vp9";
    } else if (wants_vp8 && screen_service_.SupportsVP8()) {
      negotiated_codec = "vp8";
    } else if (wants_h265 && screen_service_.SupportsH265()) {
      negotiated_codec = "h265";
    } else if (wants_h264 && screen_service_.SupportsH264()) {
      negotiated_codec = "h264";
    }
    const int requested_fps =
        std::clamp(JsonInt(payload, "fps", kNativeCaptureFps), kNativeCaptureMinFps, kNativeCaptureMaxFps);
    const int requested_scale_percent = ParseNativeScalePercent(
        JsonString(payload, "resolution_tier"),
        std::clamp(JsonInt(payload, "scale_percent", kNativeScaleDefaultPercent),
                   kNativeScaleMinPercent, kNativeScaleMaxPercent));
    const int requested_video_bitrate_bps = ParseNativeBitrateBps(
        JsonString(payload, "bitrate_tier"),
        std::clamp(JsonInt(payload, "bitrate_bps", kNativeBitrateDefaultBps),
                   kNativeBitrateMinBps, kNativeBitrateMaxBps));
    std::string source_error;
    const std::string normalized_source_id =
        screen_service_.NormalizeCaptureSourceId(requested_source_id, &source_error);
    if (normalized_source_id.empty()) {
      SendToWs(channel_key,
               api::Error(source_error.empty() ? "no capture source available" : source_error));
      return;
    }

    {
      std::lock_guard<std::mutex> lock(ws_mu_);
      auto it = ws_clients_.find(channel_key);
      if (it != ws_clients_.end()) {
        it->second.native_stream_subscribed = true;
        it->second.native_stream_codec = negotiated_codec;
        it->second.native_stream_source_id = normalized_source_id;
        it->second.native_stream_fps = requested_fps;
        it->second.native_stream_scale_percent = requested_scale_percent;
        it->second.native_stream_video_bitrate_bps = requested_video_bitrate_bps;
      }
    }

    RefreshNativeCaptureState(client.session_token);
    const int effective_fps = active_capture_fps_.load();
    const int effective_scale_percent = active_capture_scale_percent_.load();
    const int effective_video_bitrate_bps = active_capture_video_bitrate_bps_.load();
    const std::string effective_source_id = [this, &normalized_source_id]() {
      const std::string active = screen_service_.ActiveCaptureSourceId();
      return active.empty() ? normalized_source_id : active;
    }();

    SendToWs(channel_key, api::Success({
        {"event", "native_subscribed", false},
        {"capture_running", screen_service_.IsCapturing() ? "true" : "false", true},
        {"codec", negotiated_codec, false},
        {"source_id", effective_source_id, false},
        {"fps", std::to_string(effective_fps > 0 ? effective_fps : requested_fps), true},
        {"resolution_tier", NativeResolutionTierFromScale(effective_scale_percent), false},
        {"scale_percent", std::to_string(effective_scale_percent), true},
        {"bitrate_tier", NativeBitrateTierFromBps(effective_video_bitrate_bps), false},
        {"bitrate_bps", std::to_string(effective_video_bitrate_bps), true},
        {"transport", "ws-binary", false},
    }));
    return;
  }

  if (action == "native_unsubscribe") {
    {
      std::lock_guard<std::mutex> lock(ws_mu_);
      auto it = ws_clients_.find(channel_key);
      if (it != ws_clients_.end()) {
        it->second.native_stream_subscribed = false;
        it->second.native_stream_source_id.clear();
      }
    }

    RefreshNativeCaptureState(client.session_token);

    SendToWs(channel_key, api::Success({
        {"event", "native_unsubscribed", false},
    }));
    return;
  }

  if (action == "input_event") {
    const std::string type = JsonString(payload, "type");
    std::string event_payload = JsonString(payload, "payload");
    if (payload->contains("payload") && !(*payload)["payload"].is_string()) {
      event_payload = (*payload)["payload"].dump();
    }

    std::string error;
    if (!screen_service_.InjectInputEvent(client.session_token, {type, event_payload}, &error)) {
      SendToWs(channel_key, api::Error(error, "forbidden"));
      return;
    }

    audit_logger_.Append(client.session_token, "screen.input.ws", type);
    SendToWs(channel_key, api::Success({
        {"event", "input_ack", false},
        {"message", error, false},
    }));
    return;
  }

  SendToWs(channel_key, api::Error("unknown webrtc action"));
  audit_logger_.Append(client.session_token, "webrtc.ws.invalid_action", action);
}

void ServerApp::HandleLogsWsMessage(std::uintptr_t channel_key, const std::string& message) {
  const auto payload = ParseJsonOrNull(message);
  if (!payload.has_value() || !payload->is_object()) {
    SendToWs(channel_key, api::Error("invalid json payload"));
    return;
  }

  WsClient client;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    auto it = ws_clients_.find(channel_key);
    if (it == ws_clients_.end()) {
      return;
    }
    client = it->second;
  }

  auto session = session_manager_.GetSession(client.session_token);
  if (!session.has_value()) {
    SendToWs(channel_key, api::Error("session expired", "unauthorized"));
    return;
  }
  (void)session;

  const std::string action = JsonString(payload, "action", "tail");
  if (action == "tail" || action == "snapshot") {
    const int lines = std::clamp(JsonInt(payload, "lines", 300), 1, 2000);
    SendToWs(channel_key, api::Success({
        {"event", "logs_snapshot", false},
        {"items", audit_logger_.Tail(static_cast<size_t>(lines)), true},
    }));
    return;
  }

  SendToWs(channel_key, api::Error("unknown logs action"));
}

void ServerApp::HandleDockurrWsMessage(std::uintptr_t channel_key, const std::string& message) {
  const auto payload = ParseJsonOrNull(message);
  if (!payload.has_value() || !payload->is_object()) {
    SendToWs(channel_key, api::Error("invalid json payload"));
    return;
  }

  WsClient client;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    auto it = ws_clients_.find(channel_key);
    if (it == ws_clients_.end()) {
      return;
    }
    client = it->second;
  }

  auto session = session_manager_.GetSession(client.session_token);
  if (!session.has_value()) {
    SendToWs(channel_key, api::Error("session expired", "unauthorized"));
    return;
  }
  (void)session;

  const std::string action = JsonString(payload, "action", "list");
  const std::string request_id = JsonString(payload, "request_id");
  const auto send_runtime_log =
      [this, channel_key, &request_id](const std::string& level, const std::string& action_name,
                                       const std::string& detail) {
        SendToWs(channel_key, BuildDockurrRuntimeLogPayload(level, action_name, detail, request_id));
      };

  const auto send_action_result =
      [this, channel_key, &action, &request_id](bool success, const std::string& error,
                                                const std::vector<util::JsonField>& extra_fields = {}) {
        std::vector<util::JsonField> fields{
            {"event", "dockurr_action_result", false},
            {"action", action, false},
            {"request_id", request_id, false},
            {"success", success ? "true" : "false", true},
        };
        if (!error.empty()) {
          fields.push_back({"error", error, false});
        }
        fields.insert(fields.end(), extra_fields.begin(), extra_fields.end());
        SendToWs(channel_key, api::Success(fields));
      };

  if (action == "list" || action == "snapshot") {
    std::string error;
    const auto vms = dockurr_manager_.ListVms(&error);
    if (!error.empty()) {
      send_runtime_log("error", action, error);
      send_action_result(false, error);
      return;
    }
    SendToWs(channel_key, BuildDockurrSnapshotPayload(vms));
    send_runtime_log("info", action, "snapshot updated (" + std::to_string(vms.size()) + " vm)");
    send_action_result(true, "");
    return;
  }

  if (action == "start" || action == "stop" || action == "restart" || action == "logs" || action == "inspect") {
    const std::string name = JsonString(payload, "name");
    if (name.empty()) {
      send_runtime_log("error", action, "name is required");
      send_action_result(false, "name is required");
      return;
    }

    send_runtime_log("info", action, "executing for vm: " + name);
    std::string error;
    if (action == "start") {
      if (!dockurr_manager_.StartVm(name, &error)) {
        send_runtime_log("error", action, error);
        send_action_result(false, error);
        return;
      }
      audit_logger_.Append(client.session_token, "dockurr.start", name);
      send_runtime_log("info", action, "vm started: " + name);
      send_action_result(true, "", {{"name", name, false}});
    } else if (action == "stop") {
      if (!dockurr_manager_.StopVm(name, &error)) {
        send_runtime_log("error", action, error);
        send_action_result(false, error);
        return;
      }
      audit_logger_.Append(client.session_token, "dockurr.stop", name);
      send_runtime_log("info", action, "vm stopped: " + name);
      send_action_result(true, "", {{"name", name, false}});
    } else if (action == "restart") {
      if (!dockurr_manager_.RestartVm(name, &error)) {
        send_runtime_log("error", action, error);
        send_action_result(false, error);
        return;
      }
      audit_logger_.Append(client.session_token, "dockurr.restart", name);
      send_runtime_log("info", action, "vm restarted: " + name);
      send_action_result(true, "", {{"name", name, false}});
    } else if (action == "logs") {
      const int tail = std::clamp(JsonInt(payload, "tail", 50), 1, 500);
      std::string logs;
      if (!dockurr_manager_.GetLogs(name, tail, &logs, &error)) {
        send_runtime_log("error", action, error);
        send_action_result(false, error);
        return;
      }
      send_runtime_log("info", action, "fetched " + std::to_string(tail) + " lines for " + name);
      send_action_result(true, "", {
                                      {"name", name, false},
                                      {"logs", logs, false},
                                  });
    } else {
      std::string inspect;
      if (!dockurr_manager_.InspectVm(name, &inspect, &error)) {
        send_runtime_log("error", action, error);
        send_action_result(false, error);
        return;
      }
      send_runtime_log("info", action, "inspect loaded for " + name);
      send_action_result(true, "", {
                                      {"name", name, false},
                                      {"inspect", inspect, false},
                                  });
    }

    std::string list_error;
    const auto vms = dockurr_manager_.ListVms(&list_error);
    if (list_error.empty()) {
      SendToWs(channel_key, BuildDockurrSnapshotPayload(vms));
    } else {
      send_runtime_log("warn", action, "post-action snapshot failed: " + list_error);
    }
    return;
  }

  if (action == "create") {
    dockurr::CreateVmRequest create_request;
    create_request.os = ToLower(JsonString(payload, "os"));
    create_request.version = JsonString(payload, "version");
    create_request.ram_size = JsonString(payload, "ram", JsonString(payload, "ram_size", "4G"));
    create_request.disk_size = JsonString(payload, "disk", JsonString(payload, "disk_size", "64G"));
    create_request.name = JsonString(payload, "name");
    create_request.persistent = JsonBool(payload, "persist", JsonBool(payload, "persistent", false));
    if (create_request.os.empty()) {
      send_runtime_log("error", action, "os is required");
      send_action_result(false, "os is required");
      return;
    }

    send_runtime_log("info", action,
                     "accepted request: os=" + create_request.os + ", version=" + create_request.version +
                         ", ram=" + create_request.ram_size +
                         ", disk=" + create_request.disk_size +
                         ", persist=" + (create_request.persistent ? "true" : "false"));
    send_action_result(true, "", {
                                     {"accepted", "true", true},
                                 });

    std::thread([this, channel_key, request_id, create_request, session_token = client.session_token]() {
      const auto emit_runtime_log = [this, channel_key, request_id](const std::string& level,
                                                                     const std::string& action_name,
                                                                     const std::string& detail) {
        SendToWs(channel_key, BuildDockurrRuntimeLogPayload(level, action_name, detail, request_id));
      };
      const auto emit_startup_line = [this, channel_key, &request_id](const std::string& line) {
        SendToWs(channel_key, api::Success({
                                 {"event", "dockurr_startup_log", false},
                                 {"ts", util::UtcNowIso8601(), false},
                                 {"request_id", request_id, false},
                                 {"line", line, false},
                             }));
      };

      dockurr::VmInfo vm;
      std::string create_error;
      const bool created = dockurr_manager_.CreateVmWithStartupLogs(
          create_request, kDockurrCreateLogWaitSeconds,
          [emit_startup_line, emit_runtime_log](const std::string& line) {
            emit_startup_line(line);
            emit_runtime_log("info", "create.startup", line);
          },
          &vm, &create_error);
      if (!created) {
        emit_runtime_log("error", "create", create_error);
        SendToWs(channel_key, api::Success({
                                 {"event", "dockurr_action_result", false},
                                 {"action", "create", false},
                                 {"request_id", request_id, false},
                                 {"success", "false", true},
                                 {"error", create_error, false},
                             }));
      } else {
        audit_logger_.Append(session_token, "dockurr.create",
                             vm.name + " (" + create_request.os + " " + create_request.version + ")");
        emit_runtime_log("info", "create",
                         "vm created: " + vm.name + ", novnc_port=" +
                             (vm.novnc_port.empty() ? "pending" : vm.novnc_port));
        SendToWs(channel_key, api::Success({
                                 {"event", "dockurr_action_result", false},
                                 {"action", "create", false},
                                 {"request_id", request_id, false},
                                 {"success", "true", true},
                                 {"accepted", "false", true},
                                 {"vm", SerializeDockurrVm(vm), true},
                             }));
      }

      std::string list_error;
      const auto vms = dockurr_manager_.ListVms(&list_error);
      if (list_error.empty()) {
        SendToWs(channel_key, BuildDockurrSnapshotPayload(vms));
      } else {
        emit_runtime_log("warn", "create", "post-create snapshot failed: " + list_error);
      }

      SendToWs(channel_key, api::Success({
                               {"event", "dockurr_startup_done", false},
                               {"ts", util::UtcNowIso8601(), false},
                               {"request_id", request_id, false},
                               {"success", created ? "true" : "false", true},
                           }));
    }).detach();
    return;
  }

  send_runtime_log("error", action, "unknown dockurr action");
  send_action_result(false, "unknown dockurr action");
}

void ServerApp::HandleMonitorWsMessage(std::uintptr_t channel_key, const std::string& message) {
  const auto payload = ParseJsonOrNull(message);
  if (!payload.has_value() || !payload->is_object()) {
    SendToWs(channel_key, api::Error("invalid json payload"));
    return;
  }

  WsClient client;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    auto it = ws_clients_.find(channel_key);
    if (it == ws_clients_.end()) {
      return;
    }
    client = it->second;
  }

  auto session = session_manager_.GetSession(client.session_token);
  if (!session.has_value()) {
    SendToWs(channel_key, api::Error("session expired", "unauthorized"));
    return;
  }
  (void)session;

  const std::string action = JsonString(payload, "action", "snapshot");
  if (action == "snapshot" || action == "refresh") {
    SendToWs(channel_key, api::Success({
        {"event", "monitor_snapshot", false},
        {"snapshot", system_monitor_.SnapshotJson(), true},
    }));
    return;
  }

  if (action == "ping") {
    SendToWs(channel_key, api::Success({
        {"event", "monitor_pong", false},
    }));
    return;
  }

  SendToWs(channel_key, api::Error("unknown monitor action"));
}

#endif

}  // namespace ferryman::web
