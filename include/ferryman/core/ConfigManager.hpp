#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace ferryman::core {

struct AppConfig {
  std::string http_host = "0.0.0.0";
  int http_port = 18080;
  bool https_enabled = false;
  int https_port = 18443;
  std::filesystem::path tls_cert_file;
  std::filesystem::path tls_key_file;
  int ws_port = 18080;
  std::string access_key;
  std::filesystem::path workspace_root;
  std::filesystem::path config_path;
  std::filesystem::path audit_log_path;
};

class ConfigManager {
 public:
  ConfigManager() = default;

  bool Initialize();
  const AppConfig& config() const { return config_; }

 private:
  bool LoadFromDisk(const std::filesystem::path& path);
  bool WriteDefaultConfig(const std::filesystem::path& path, const std::string& access_key) const;

  AppConfig config_{};
};

}  // namespace ferryman::core
