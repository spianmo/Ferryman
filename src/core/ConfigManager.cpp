#include "ferryman/core/ConfigManager.hpp"

#include "ferryman/util/Random.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

namespace ferryman::core {

namespace {

std::string ExpandHome() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || std::string(home).empty()) {
    return ".";
  }
  return std::string(home);
}

std::string Trim(std::string line) {
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
    line.pop_back();
  }
  size_t pos = 0;
  while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
    ++pos;
  }
  return line.substr(pos);
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool ParseBool(std::string value, bool default_value = false) {
  value = ToLower(Trim(value));
  if (value.empty()) {
    return default_value;
  }
  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    return false;
  }
  return default_value;
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

std::filesystem::path ResolveConfiguredPath(const std::filesystem::path& config_path, const std::string& raw_value) {
  const std::string trimmed = Trim(raw_value);
  if (trimmed.empty()) {
    return {};
  }
  std::filesystem::path path(trimmed);
  if (path.is_relative() && !config_path.empty()) {
    path = config_path.parent_path() / path;
  }
  return path;
}

std::string NormalizeTunnelProtocol(std::string protocol) {
  protocol = ToLower(Trim(protocol));
  if (protocol == "udp") {
    return "udp";
  }
  return "tcp";
}

std::vector<TunnelMappingConfig> ParseTunnelMappingsJson(const std::string& raw_json) {
  std::vector<TunnelMappingConfig> mappings;
  if (Trim(raw_json).empty()) {
    return mappings;
  }

  const auto parsed = nlohmann::json::parse(raw_json, nullptr, false);
  if (!parsed.is_array()) {
    return mappings;
  }

  for (const auto& item : parsed) {
    if (!item.is_object()) {
      continue;
    }
    TunnelMappingConfig mapping;
    if (item.contains("id") && item["id"].is_string()) {
      mapping.id = item["id"].get<std::string>();
    }
    if (mapping.id.empty()) {
      continue;
    }
    if (item.contains("name") && item["name"].is_string()) {
      mapping.name = item["name"].get<std::string>();
    }
    if (item.contains("protocol")) {
      if (item["protocol"].is_string()) {
        mapping.protocol = NormalizeTunnelProtocol(item["protocol"].get<std::string>());
      }
    }
    if (item.contains("local_host") && item["local_host"].is_string()) {
      mapping.local_host = Trim(item["local_host"].get<std::string>());
    }
    if (mapping.local_host.empty()) {
      mapping.local_host = "127.0.0.1";
    }

    if (item.contains("local_port")) {
      if (item["local_port"].is_number_integer()) {
        mapping.local_port = item["local_port"].get<int>();
      } else if (item["local_port"].is_string()) {
        mapping.local_port = ParseInt(item["local_port"].get<std::string>(), 0);
      }
    }
    if (item.contains("remote_port")) {
      if (item["remote_port"].is_number_integer()) {
        mapping.remote_port = item["remote_port"].get<int>();
      } else if (item["remote_port"].is_string()) {
        mapping.remote_port = ParseInt(item["remote_port"].get<std::string>(), 0);
      }
    }

    if (item.contains("enabled")) {
      if (item["enabled"].is_boolean()) {
        mapping.enabled = item["enabled"].get<bool>();
      } else if (item["enabled"].is_number_integer()) {
        mapping.enabled = item["enabled"].get<int>() != 0;
      } else if (item["enabled"].is_string()) {
        mapping.enabled = ParseBool(item["enabled"].get<std::string>(), true);
      }
    }

    if (mapping.local_port <= 0 || mapping.local_port > 65535) {
      continue;
    }
    if (mapping.remote_port <= 0 || mapping.remote_port > 65535) {
      continue;
    }
    mappings.push_back(std::move(mapping));
  }

  return mappings;
}

std::string SerializeTunnelMappingsJson(const std::vector<TunnelMappingConfig>& mappings) {
  nlohmann::json arr = nlohmann::json::array();
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

}  // namespace

bool ConfigManager::Initialize() {
  const std::filesystem::path home_dir = ExpandHome();
  const std::filesystem::path ferryman_home = home_dir / ".ferryman";
  const std::filesystem::path config_path = ferryman_home / "config.ini";
  const std::filesystem::path logs_dir = ferryman_home / "logs";

  std::error_code ec;
  std::filesystem::create_directories(logs_dir, ec);
  if (ec) {
    std::cerr << "[ferryman] failed to create config directory: " << ec.message() << '\n';
    return false;
  }

  if (!std::filesystem::exists(config_path)) {
    const std::string key = util::RandomHex(40);
    if (!WriteDefaultConfig(config_path, key)) {
      return false;
    }
    std::cout << "[ferryman] initialized access key at " << config_path << '\n';
    std::cout << "[ferryman] access key: " << key << '\n';
  }

  if (!LoadFromDisk(config_path)) {
    return false;
  }

  config_.config_path = config_path;
  config_.workspace_root = home_dir;
  config_.audit_log_path = logs_dir / "audit.log";

  if (config_.access_key.empty()) {
    config_.access_key = util::RandomHex(40);
    WriteDefaultConfig(config_path, config_.access_key);
  }
  return true;
}

bool ConfigManager::LoadFromDisk(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    std::cerr << "[ferryman] failed to open config file: " << path << '\n';
    return false;
  }

  bool has_https_port = false;
  std::string line;
  while (std::getline(file, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string key = Trim(line.substr(0, eq));
    const std::string value = Trim(line.substr(eq + 1));
    if (key == "access_key") {
      config_.access_key = value;
    } else if (key == "http_host") {
      config_.http_host = value;
    } else if (key == "http_port") {
      config_.http_port = ParseInt(value, config_.http_port);
    } else if (key == "https_enabled") {
      config_.https_enabled = ParseBool(value, config_.https_enabled);
    } else if (key == "https_port") {
      config_.https_port = ParseInt(value, config_.https_port);
      has_https_port = true;
    } else if (key == "tls_cert_file") {
      config_.tls_cert_file = ResolveConfiguredPath(path, value);
    } else if (key == "tls_key_file") {
      config_.tls_key_file = ResolveConfiguredPath(path, value);
    } else if (key == "ws_port") {
      config_.ws_port = ParseInt(value, config_.ws_port);
    } else if (key == "tunnel_proxy_host") {
      config_.tunnel_proxy_host = Trim(value);
    } else if (key == "tunnel_proxy_port") {
      config_.tunnel_proxy_port = ParseInt(value, config_.tunnel_proxy_port);
    } else if (key == "tunnel_mappings_json") {
      config_.tunnel_mappings = ParseTunnelMappingsJson(value);
    }
  }
  if (config_.http_port <= 0) {
    config_.http_port = 18080;
  }
  if (!has_https_port) {
    config_.https_port = config_.http_port + 1;
  } else if (config_.https_port <= 0) {
    config_.https_port = config_.http_port + 1;
  }
  if (config_.tunnel_proxy_port <= 0 || config_.tunnel_proxy_port > 65535) {
    config_.tunnel_proxy_port = 17000;
  }
  // WebSocket and HTTP now share one listener/port.
  config_.ws_port = config_.http_port;
  return true;
}

bool ConfigManager::WriteDefaultConfig(const std::filesystem::path& path, const std::string& access_key) const {
  std::ofstream file(path, std::ios::trunc);
  if (!file.is_open()) {
    std::cerr << "[ferryman] failed to write config file: " << path << '\n';
    return false;
  }
  file << "# Ferryman configuration\n";
  file << "access_key=" << access_key << '\n';
  file << "http_host=0.0.0.0\n";
  file << "http_port=18080\n";
  file << "https_enabled=false\n";
  file << "https_port=18443\n";
  file << "tls_cert_file=\n";
  file << "tls_key_file=\n";
  file << "ws_port=18080\n";
  file << "tunnel_proxy_host=\n";
  file << "tunnel_proxy_port=17000\n";
  file << "tunnel_mappings_json=" << SerializeTunnelMappingsJson({}) << '\n';
  return true;
}

}  // namespace ferryman::core
