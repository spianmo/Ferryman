#include "ferryman/proxy/ProxyServer.hpp"
#include "ferryman/util/Random.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

std::atomic<bool> g_running{true};
std::condition_variable g_cv;
std::mutex g_mu;

void HandleSignal(int signal) {
  if (signal == SIGINT || signal == SIGTERM) {
    g_running.store(false);
    g_cv.notify_all();
  }
}

std::string Trim(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
    value.pop_back();
  }
  size_t pos = 0;
  while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t')) {
    ++pos;
  }
  return value.substr(pos);
}

std::string ExpandHome() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || std::string(home).empty()) {
    return ".";
  }
  return std::string(home);
}

bool WriteProxyTokenConfig(const std::filesystem::path& path, const std::string& token, std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    if (error != nullptr) {
      *error = "failed to create config directory: " + ec.message();
    }
    return false;
  }

  std::ofstream file(path, std::ios::trunc);
  if (!file.is_open()) {
    if (error != nullptr) {
      *error = "failed to write config file: " + path.string();
    }
    return false;
  }
  file << "# FerrymanProxy configuration\n";
  file << "proxy_token=" << token << '\n';
  return true;
}

std::optional<std::string> ReadProxyTokenFromConfig(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::string line;
  while (std::getline(file, line)) {
    line = Trim(line);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string key = Trim(line.substr(0, eq));
    const std::string value = Trim(line.substr(eq + 1));
    if (key == "proxy_token") {
      return value;
    }
  }
  return std::nullopt;
}

std::optional<std::string> EnsureProxyToken(std::filesystem::path* config_path_out, std::string* error) {
  const std::filesystem::path config_path = std::filesystem::path(ExpandHome()) / ".ferryman-proxy" / "config.ini";
  if (config_path_out != nullptr) {
    *config_path_out = config_path;
  }

  const auto loaded = ReadProxyTokenFromConfig(config_path);
  if (loaded.has_value() && !loaded->empty()) {
    return *loaded;
  }

  const std::string token = ferryman::util::RandomHex(40);
  if (!WriteProxyTokenConfig(config_path, token, error)) {
    return std::nullopt;
  }
  if (!std::filesystem::exists(config_path) || !loaded.has_value()) {
    std::cout << "[ferryman-proxy] initialized token at " << config_path << '\n';
  } else {
    std::cout << "[ferryman-proxy] regenerated empty token at " << config_path << '\n';
  }
  std::cout << "[ferryman-proxy] token: " << token << '\n';
  return token;
}

void PrintUsage() {
  std::cout << "FerrymanProxy (Linux only)\n"
               "Usage:\n"
               "  FerrymanProxy [--bind 0.0.0.0] [--control-port 17000] [--admin-host 127.0.0.1] [--admin-port 17001]\n"
               "                [--log-file /var/log/ferryman-proxy.log] [--token TOKEN]\n"
               "  FerrymanProxy --list [--admin-host 127.0.0.1] [--admin-port 17001] [--token TOKEN]\n"
               "  FerrymanProxy --status [--admin-host 127.0.0.1] [--admin-port 17001] [--token TOKEN]\n"
               "  FerrymanProxy --logs [N] [--admin-host 127.0.0.1] [--admin-port 17001] [--token TOKEN]  (streaming)\n";
}

#if defined(__linux__)
int ConnectTcp(const std::string& host, int port, std::string* error) {
  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  struct addrinfo* result = nullptr;
  const std::string port_text = std::to_string(port);
  const int gai = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result);
  if (gai != 0) {
    if (error != nullptr) {
      *error = std::string("getaddrinfo failed: ") + gai_strerror(gai);
    }
    return -1;
  }

  int fd = -1;
  for (struct addrinfo* it = result; it != nullptr; it = it->ai_next) {
    fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) {
      continue;
    }
    if (::connect(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen)) == 0) {
      break;
    }
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(result);

  if (fd < 0 && error != nullptr && error->empty()) {
    *error = "connect failed";
  }
  return fd;
}

std::optional<std::string> QueryAdmin(const std::string& host, int port, const std::string& command,
                                      const std::string& auth_token, std::string* error) {
  int fd = ConnectTcp(host, port, error);
  if (fd < 0) {
    return std::nullopt;
  }
  std::string request;
  if (!auth_token.empty()) {
    request += "TOKEN " + auth_token + " ";
  }
  request += command;
  request.push_back('\n');
  if (::send(fd, request.data(), request.size(), 0) != static_cast<ssize_t>(request.size())) {
    ::close(fd);
    if (error != nullptr) {
      *error = "failed to send command";
    }
    return std::nullopt;
  }
  std::string response;
  char buf[4096];
  while (true) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      break;
    }
    response.append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  if (response.empty()) {
    if (error != nullptr) {
      *error = "empty response";
    }
    return std::nullopt;
  }
  return response;
}

void PrintMappingsTable(const std::string& raw_json) {
  const auto payload = nlohmann::json::parse(raw_json, nullptr, false);
  if (payload.is_discarded()) {
    std::cout << raw_json << '\n';
    return;
  }
  const auto items = payload.value("mappings", nlohmann::json::array());
  if (!items.is_array() || items.empty()) {
    std::cout << "No mappings\n";
    return;
  }

  std::cout << "mapping_id\tprotocol\tremote\tlocal\tclient\tenabled\tactive\n";
  for (const auto& item : items) {
    if (!item.is_object()) {
      continue;
    }
    std::cout << item.value("mapping_id", "") << '\t' << item.value("protocol", "") << '\t'
              << item.value("remote_port", 0) << '\t' << item.value("local_host", "") << ':'
              << item.value("local_port", 0) << '\t' << item.value("client_id", "") << '\t'
              << (item.value("enabled", true) ? "yes" : "no") << '\t'
              << (item.value("active", false) ? "yes" : "no") << '\n';
  }
}

std::vector<std::string> ParseLogItems(const std::string& raw_json) {
  std::vector<std::string> items;
  const auto payload = nlohmann::json::parse(raw_json, nullptr, false);
  if (payload.is_discarded() || !payload.is_object()) {
    return items;
  }
  const auto entries = payload.value("items", nlohmann::json::array());
  if (!entries.is_array()) {
    return items;
  }
  items.reserve(entries.size());
  for (const auto& entry : entries) {
    if (entry.is_string()) {
      items.push_back(entry.get<std::string>());
    } else {
      items.push_back(entry.dump());
    }
  }
  return items;
}

size_t FindLogOverlap(const std::vector<std::string>& previous, const std::vector<std::string>& current) {
  const size_t max_overlap = std::min(previous.size(), current.size());
  for (size_t overlap = max_overlap; overlap > 0; --overlap) {
    bool same = true;
    for (size_t idx = 0; idx < overlap; ++idx) {
      if (previous[previous.size() - overlap + idx] != current[idx]) {
        same = false;
        break;
      }
    }
    if (same) {
      return overlap;
    }
  }
  return 0;
}
#endif

}  // namespace

int main(int argc, char** argv) {
#if !defined(__linux__)
  (void)argc;
  (void)argv;
  std::cerr << "FerrymanProxy is only supported on Linux.\n";
  return 1;
#else
  ferryman::proxy::ProxyServerOptions options;
  std::string mode = "serve";
  size_t logs_limit = 100;
  std::string auth_token_override;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      PrintUsage();
      return 0;
    }
    if (arg == "--bind" && i + 1 < argc) {
      options.bind_host = argv[++i];
      continue;
    }
    if (arg == "--control-port" && i + 1 < argc) {
      options.control_port = std::atoi(argv[++i]);
      continue;
    }
    if (arg == "--admin-host" && i + 1 < argc) {
      options.admin_host = argv[++i];
      continue;
    }
    if (arg == "--admin-port" && i + 1 < argc) {
      options.admin_port = std::atoi(argv[++i]);
      continue;
    }
    if (arg == "--log-file" && i + 1 < argc) {
      options.log_file = argv[++i];
      continue;
    }
    if (arg == "--token" && i + 1 < argc) {
      auth_token_override = Trim(argv[++i]);
      continue;
    }
    if (arg == "--list") {
      mode = "list";
      continue;
    }
    if (arg == "--status") {
      mode = "status";
      continue;
    }
    if (arg == "--logs") {
      mode = "logs";
      if (i + 1 < argc) {
        const std::string next = argv[i + 1];
        if (!next.empty() && next.front() != '-') {
          logs_limit = static_cast<size_t>(std::max(1, std::atoi(next.c_str())));
          ++i;
        }
      }
      continue;
    }
    std::cerr << "Unknown argument: " << arg << '\n';
    PrintUsage();
    return 1;
  }

  if (!auth_token_override.empty()) {
    options.auth_token = auth_token_override;
  } else {
    std::string token_error;
    const auto token = EnsureProxyToken(nullptr, &token_error);
    if (!token.has_value() || token->empty()) {
      std::cerr << "failed to load proxy token: " << (token_error.empty() ? "unknown error" : token_error) << '\n';
      return 1;
    }
    options.auth_token = *token;
  }

  if (mode != "serve") {
    if (mode == "logs") {
      std::signal(SIGINT, HandleSignal);
      std::signal(SIGTERM, HandleSignal);
      std::vector<std::string> previous_items;
      while (g_running.load()) {
        std::string error;
        const auto response =
            QueryAdmin(options.admin_host, options.admin_port, "LOGS " + std::to_string(logs_limit), options.auth_token, &error);
        if (!response.has_value()) {
          std::cerr << "admin query failed: " << (error.empty() ? "unknown error" : error) << '\n';
          std::this_thread::sleep_for(std::chrono::milliseconds(800));
          continue;
        }
        const auto current_items = ParseLogItems(*response);
        if (!current_items.empty()) {
          const size_t overlap = FindLogOverlap(previous_items, current_items);
          for (size_t i = overlap; i < current_items.size(); ++i) {
            std::cout << current_items[i] << '\n';
          }
          std::cout.flush();
          previous_items = current_items;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
      }
      return 0;
    }

    std::string error;
    std::string command = "LIST";
    if (mode == "status") {
      command = "STATUS";
    }
    const auto response = QueryAdmin(options.admin_host, options.admin_port, command, options.auth_token, &error);
    if (!response.has_value()) {
      std::cerr << "admin query failed: " << (error.empty() ? "unknown error" : error) << '\n';
      return 1;
    }
    if (mode == "list") {
      PrintMappingsTable(*response);
    } else {
      std::cout << *response << '\n';
    }
    return 0;
  }

  if (options.control_port <= 0 || options.control_port > 65535 || options.admin_port <= 0 || options.admin_port > 65535) {
    std::cerr << "invalid port configuration\n";
    return 1;
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  ferryman::proxy::ProxyServer server(options);
  std::string error;
  if (!server.Start(&error)) {
    std::cerr << "failed to start FerrymanProxy: " << (error.empty() ? "unknown error" : error) << '\n';
    return 1;
  }

  {
    std::unique_lock<std::mutex> lock(g_mu);
    g_cv.wait(lock, [] {
      return !g_running.load();
    });
  }

  server.Stop();
  return 0;
#endif
}
