#include "ferryman/core/ConfigManager.hpp"

#include "ferryman/util/Random.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
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
      config_.http_port = std::stoi(value);
    } else if (key == "ws_port") {
      config_.ws_port = std::stoi(value);
    }
  }
  if (config_.http_port <= 0) {
    config_.http_port = 18080;
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
  file << "ws_port=18080\n";
  return true;
}

}  // namespace ferryman::core
