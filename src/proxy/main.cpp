#include "ferryman/proxy/ProxyServer.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
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

void PrintUsage() {
  std::cout << "FerrymanProxy (Linux only)\n"
               "Usage:\n"
               "  FerrymanProxy [--bind 0.0.0.0] [--control-port 17000] [--admin-host 127.0.0.1] [--admin-port 17001]\n"
               "                [--log-file /var/log/ferryman-proxy.log]\n"
               "  FerrymanProxy --list [--admin-host 127.0.0.1] [--admin-port 17001]\n"
               "  FerrymanProxy --status [--admin-host 127.0.0.1] [--admin-port 17001]\n"
               "  FerrymanProxy --logs [N] [--admin-host 127.0.0.1] [--admin-port 17001]\n";
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

std::optional<std::string> QueryAdmin(const std::string& host, int port, const std::string& command, std::string* error) {
  int fd = ConnectTcp(host, port, error);
  if (fd < 0) {
    return std::nullopt;
  }
  std::string request = command;
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
    std::cout << "No active mappings\n";
    return;
  }

  std::cout << "mapping_id\tprotocol\tremote\tlocal\tclient\tactive\n";
  for (const auto& item : items) {
    if (!item.is_object()) {
      continue;
    }
    std::cout << item.value("mapping_id", "") << '\t' << item.value("protocol", "") << '\t'
              << item.value("remote_port", 0) << '\t' << item.value("local_host", "") << ':'
              << item.value("local_port", 0) << '\t' << item.value("client_id", "") << '\t'
              << (item.value("active", false) ? "yes" : "no") << '\n';
  }
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

  if (mode != "serve") {
    std::string error;
    std::string command = "LIST";
    if (mode == "status") {
      command = "STATUS";
    } else if (mode == "logs") {
      command = "LOGS " + std::to_string(logs_limit);
    }
    const auto response = QueryAdmin(options.admin_host, options.admin_port, command, &error);
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
