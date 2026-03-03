#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ferryman::core {

struct TunnelMappingConfig {
  std::string id;
  std::string name;
  std::string protocol = "tcp";
  std::string local_host = "127.0.0.1";
  int local_port = 0;
  int remote_port = 0;
  bool enabled = true;
};

struct AppConfig {
  std::string http_host = "0.0.0.0";
  int http_port = 18080;
  bool https_enabled = false;
  int https_port = 18443;
  std::filesystem::path tls_cert_file;
  std::filesystem::path tls_key_file;
  int codeserver_port = 13337;
  bool codeserver_https_enabled = true;
  std::string codeserver_https_mode = "ferryman";
  std::filesystem::path codeserver_https_cert_file;
  std::filesystem::path codeserver_https_key_file;
  bool has_codeserver_port = false;
  bool has_codeserver_https_enabled = false;
  bool has_codeserver_https_mode = false;
  bool has_codeserver_https_cert_file = false;
  bool has_codeserver_https_key_file = false;
  int ws_port = 18080;
  std::string access_key;
  std::filesystem::path workspace_root;
  std::filesystem::path config_path;
  std::filesystem::path audit_log_path;
  std::string tunnel_proxy_host;
  int tunnel_proxy_port = 17000;
  std::string tunnel_proxy_token;
  std::vector<TunnelMappingConfig> tunnel_mappings;
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
