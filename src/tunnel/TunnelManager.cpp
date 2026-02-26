#include "ferryman/tunnel/TunnelManager.hpp"

#include "ferryman/core/AuditLogger.hpp"
#include "ferryman/tunnel/PortInspector.hpp"
#include "ferryman/util/Random.hpp"
#include "ferryman/util/StringUtil.hpp"
#include "ferryman/util/Time.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ferryman::tunnel {

namespace {

using nlohmann::json;

constexpr int kDefaultProxyPort = 17000;
constexpr int kControlPingIntervalSeconds = 15;
constexpr int kControlSleepWhenIdleMs = 800;
constexpr int kControlReconnectDelayMs = 1200;
constexpr int kUdpResponseTimeoutMs = 800;

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string NormalizeProtocol(std::string protocol) {
  protocol = ToLower(util::Trim(protocol));
  if (protocol == "udp") {
    return "udp";
  }
  return "tcp";
}

core::TunnelMappingConfig NormalizeMapping(core::TunnelMappingConfig mapping) {
  mapping.protocol = NormalizeProtocol(mapping.protocol);
  mapping.local_host = util::Trim(mapping.local_host);
  if (mapping.local_host.empty()) {
    mapping.local_host = "127.0.0.1";
  }
  if (mapping.name.empty()) {
    mapping.name = mapping.id;
  }
  return mapping;
}

bool MappingValid(const core::TunnelMappingConfig& mapping) {
  if (mapping.id.empty()) {
    return false;
  }
  if (mapping.local_port <= 0 || mapping.local_port > 65535) {
    return false;
  }
  if (mapping.remote_port <= 0 || mapping.remote_port > 65535) {
    return false;
  }
  const std::string protocol = NormalizeProtocol(mapping.protocol);
  return protocol == "tcp" || protocol == "udp";
}

std::string RuntimeNow() {
  return util::UtcNowIso8601();
}

bool JsonBoolValue(const json& payload, const char* key, bool fallback = false) {
  if (!payload.contains(key)) {
    return fallback;
  }
  const auto& value = payload[key];
  if (value.is_boolean()) {
    return value.get<bool>();
  }
  if (value.is_number_integer()) {
    return value.get<int>() != 0;
  }
  if (value.is_string()) {
    const std::string text = ToLower(util::Trim(value.get<std::string>()));
    return text == "1" || text == "true" || text == "yes" || text == "on";
  }
  return fallback;
}

std::string JsonStringValue(const json& payload, const char* key, const std::string& fallback = "") {
  if (!payload.contains(key)) {
    return fallback;
  }
  const auto& value = payload[key];
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<long long>());
  }
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<unsigned long long>());
  }
  if (value.is_boolean()) {
    return value.get<bool>() ? "true" : "false";
  }
  return fallback;
}

std::optional<std::pair<std::string, int>> ParsePeerEndpoint(const std::string& peer) {
  std::string endpoint = util::Trim(peer);
  if (endpoint.empty()) {
    return std::nullopt;
  }

  if (endpoint.front() == '[') {
    const size_t close = endpoint.find(']');
    if (close != std::string::npos && close + 2 <= endpoint.size() && endpoint[close + 1] == ':') {
      const std::string host = endpoint.substr(1, close - 1);
      int port = 0;
      try {
        port = std::stoi(endpoint.substr(close + 2));
      } catch (...) {
        return std::nullopt;
      }
      if (port > 0 && port <= 65535) {
        return std::make_pair(host, port);
      }
    }
    return std::nullopt;
  }

  const size_t colon = endpoint.rfind(':');
  if (colon == std::string::npos) {
    return std::nullopt;
  }
  const std::string host = endpoint.substr(0, colon);
  int port = 0;
  try {
    port = std::stoi(endpoint.substr(colon + 1));
  } catch (...) {
    return std::nullopt;
  }
  if (host.empty() || port <= 0 || port > 65535) {
    return std::nullopt;
  }
  return std::make_pair(host, port);
}

std::string BuildPeerEndpoint(const std::string& host, int port) {
  if (host.find(':') != std::string::npos) {
    return "[" + host + "]:" + std::to_string(port);
  }
  return host + ":" + std::to_string(port);
}

#if !defined(_WIN32)
void CloseSocket(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

bool SetNonBlocking(int fd, bool enabled) {
  const int current = ::fcntl(fd, F_GETFL, 0);
  if (current < 0) {
    return false;
  }
  int next = current;
  if (enabled) {
    next |= O_NONBLOCK;
  } else {
    next &= ~O_NONBLOCK;
  }
  return ::fcntl(fd, F_SETFL, next) == 0;
}

bool SendAll(int fd, const char* data, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    const ssize_t n = ::send(fd, data + sent, length - sent, 0);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

int ConnectTcpWithTimeout(const std::string& host, int port, int timeout_ms, std::string* error) {
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

  int connected_fd = -1;
  for (struct addrinfo* it = result; it != nullptr; it = it->ai_next) {
    const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) {
      continue;
    }
    if (!SetNonBlocking(fd, true)) {
      CloseSocket(fd);
      continue;
    }

    const int rc = ::connect(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen));
    if (rc == 0) {
      SetNonBlocking(fd, false);
      connected_fd = fd;
      break;
    }
    if (errno != EINPROGRESS) {
      CloseSocket(fd);
      continue;
    }

    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(fd, &write_fds);
    struct timeval timeout {};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    const int selected = ::select(fd + 1, nullptr, &write_fds, nullptr, &timeout);
    if (selected > 0 && FD_ISSET(fd, &write_fds)) {
      int socket_error = 0;
      socklen_t socket_error_len = sizeof(socket_error);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len) == 0 && socket_error == 0) {
        SetNonBlocking(fd, false);
        connected_fd = fd;
        break;
      }
    }
    CloseSocket(fd);
  }
  ::freeaddrinfo(result);

  if (connected_fd < 0 && error != nullptr && error->empty()) {
    *error = "connect failed";
  }
  return connected_fd;
}

bool TcpProbe(const std::string& host, int port, int timeout_ms, std::string* detail) {
  std::string error;
  const int fd = ConnectTcpWithTimeout(host, port, timeout_ms, &error);
  if (fd < 0) {
    if (detail != nullptr) {
      *detail = error.empty() ? "connect failed" : error;
    }
    return false;
  }
  CloseSocket(fd);
  if (detail != nullptr) {
    *detail = "ok";
  }
  return true;
}

void BridgeOneWay(int from_fd, int to_fd, std::atomic<bool>* stop_flag) {
  std::array<char, 16 * 1024> buffer{};
  while (!stop_flag->load()) {
    const ssize_t n = ::recv(from_fd, buffer.data(), buffer.size(), 0);
    if (n <= 0) {
      break;
    }
    if (!SendAll(to_fd, buffer.data(), static_cast<size_t>(n))) {
      break;
    }
  }
  stop_flag->store(true);
  ::shutdown(to_fd, SHUT_WR);
  ::shutdown(from_fd, SHUT_RD);
}

bool SendUdpDatagram(const std::string& host, int port, const std::string& payload, std::string* error) {
  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  struct addrinfo* result = nullptr;
  const std::string port_text = std::to_string(port);
  const int gai = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result);
  if (gai != 0) {
    if (error != nullptr) {
      *error = std::string("getaddrinfo failed: ") + gai_strerror(gai);
    }
    return false;
  }

  bool ok = false;
  for (struct addrinfo* it = result; it != nullptr; it = it->ai_next) {
    const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) {
      continue;
    }
    const ssize_t sent = ::sendto(fd, payload.data(), payload.size(), 0, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen));
    CloseSocket(fd);
    if (sent == static_cast<ssize_t>(payload.size())) {
      ok = true;
      break;
    }
  }
  ::freeaddrinfo(result);

  if (!ok && error != nullptr && error->empty()) {
    *error = "sendto failed";
  }
  return ok;
}

std::optional<std::string> UdpRequestResponse(const std::string& target_host, int target_port, const std::string& payload,
                                              int timeout_ms) {
  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  struct addrinfo* result = nullptr;
  const std::string port_text = std::to_string(target_port);
  const int gai = ::getaddrinfo(target_host.c_str(), port_text.c_str(), &hints, &result);
  if (gai != 0) {
    return std::nullopt;
  }

  std::optional<std::string> response;
  for (struct addrinfo* it = result; it != nullptr && !response.has_value(); it = it->ai_next) {
    const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) {
      continue;
    }
    if (::connect(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen)) != 0) {
      CloseSocket(fd);
      continue;
    }
    const ssize_t sent = ::send(fd, payload.data(), payload.size(), 0);
    if (sent != static_cast<ssize_t>(payload.size())) {
      CloseSocket(fd);
      continue;
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);
    struct timeval timeout {};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    const int selected = ::select(fd + 1, &read_fds, nullptr, nullptr, &timeout);
    if (selected > 0 && FD_ISSET(fd, &read_fds)) {
      std::array<char, 65535> buf{};
      const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
      if (n > 0) {
        response = std::string(buf.data(), static_cast<size_t>(n));
      } else if (n == 0) {
        response = std::string();
      }
    }
    CloseSocket(fd);
  }
  ::freeaddrinfo(result);
  return response;
}
#endif

}  // namespace

struct TunnelManager::Impl {
  explicit Impl(core::AuditLogger* logger_in) : logger(logger_in) {}

  struct PendingTest {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::string detail;
  };

  core::AuditLogger* logger = nullptr;

  mutable std::mutex mu;
  std::string proxy_host;
  int proxy_port = kDefaultProxyPort;
  std::vector<core::TunnelMappingConfig> mappings;
  std::unordered_map<std::string, TunnelMappingRuntime> runtime_by_id;
  bool sync_needed = false;

  std::atomic<bool> running{false};
  std::thread worker;
  std::string client_id = "ferryman-" + util::RandomHex(10);

#if !defined(_WIN32)
  std::mutex send_mu;
  int control_fd = -1;
  bool control_connected = false;
  std::string control_buffer;
#endif

  std::mutex tests_mu;
  std::unordered_map<std::string, std::shared_ptr<PendingTest>> pending_tests;

  void LogSystem(const std::string& level, const std::string& action, const std::string& detail) const {
    if (logger != nullptr) {
      logger->AppendSystem(level, action, detail);
    }
  }

  void SetRuntime(const std::string& mapping_id, bool active, const std::string& status, const std::string& detail) {
    if (mapping_id.empty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mu);
    auto& runtime = runtime_by_id[mapping_id];
    runtime.active = active;
    runtime.status = status;
    runtime.detail = detail;
    runtime.updated_at = RuntimeNow();
  }

  void FailAllPendingTests(const std::string& detail) {
    std::unordered_map<std::string, std::shared_ptr<PendingTest>> pending;
    {
      std::lock_guard<std::mutex> lock(tests_mu);
      pending.swap(pending_tests);
    }
    for (const auto& [_, item] : pending) {
      std::lock_guard<std::mutex> item_lock(item->mu);
      item->done = true;
      item->ok = false;
      item->detail = detail;
      item->cv.notify_all();
    }
  }

  void Configure(const std::string& host, int port, const std::vector<core::TunnelMappingConfig>& next_mappings) {
    std::unordered_map<std::string, TunnelMappingRuntime> previous_runtime;
    {
      std::lock_guard<std::mutex> lock(mu);
      proxy_host = util::Trim(host);
      proxy_port = (port > 0 && port <= 65535) ? port : kDefaultProxyPort;

      previous_runtime = runtime_by_id;
      mappings.clear();
      runtime_by_id.clear();
      mappings.reserve(next_mappings.size());
      for (const auto& raw : next_mappings) {
        core::TunnelMappingConfig normalized = NormalizeMapping(raw);
        if (!MappingValid(normalized)) {
          continue;
        }
        mappings.push_back(normalized);
        auto it = previous_runtime.find(normalized.id);
        if (it != previous_runtime.end()) {
          runtime_by_id[normalized.id] = it->second;
        } else {
          runtime_by_id[normalized.id] = TunnelMappingRuntime{
              .active = false,
              .status = normalized.enabled ? "pending" : "disabled",
              .detail = normalized.enabled ? "waiting for proxy synchronization" : "mapping disabled",
              .updated_at = RuntimeNow(),
          };
        }
      }
      sync_needed = true;
    }
  }

#if defined(_WIN32)
  void RunWorker() {
    while (running.load()) {
      {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& mapping : mappings) {
          auto& runtime = runtime_by_id[mapping.id];
          runtime.active = false;
          runtime.status = "unsupported";
          runtime.detail = "tunnel is not supported on this platform";
          runtime.updated_at = RuntimeNow();
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    }
  }
#else
  bool SendControlJson(const json& payload) {
    const std::string line = payload.dump() + "\n";
    std::lock_guard<std::mutex> lock(send_mu);
    if (control_fd < 0) {
      return false;
    }
    return SendAll(control_fd, line.data(), line.size());
  }

  bool SendSyncMappings() {
    json mappings_payload = json::array();
    std::vector<core::TunnelMappingConfig> local_mappings;
    {
      std::lock_guard<std::mutex> lock(mu);
      local_mappings = mappings;
      sync_needed = false;
    }
    for (const auto& mapping : local_mappings) {
      mappings_payload.push_back({
          {"id", mapping.id},
          {"name", mapping.name},
          {"protocol", NormalizeProtocol(mapping.protocol)},
          {"local_host", mapping.local_host},
          {"local_port", mapping.local_port},
          {"remote_port", mapping.remote_port},
          {"enabled", mapping.enabled},
      });
      if (!mapping.enabled) {
        SetRuntime(mapping.id, false, "disabled", "mapping disabled");
      } else {
        SetRuntime(mapping.id, false, "syncing", "waiting proxy apply");
      }
    }
    return SendControlJson({
        {"type", "sync_mappings"},
        {"mappings", mappings_payload},
    });
  }

  std::optional<core::TunnelMappingConfig> FindMappingById(const std::string& mapping_id) const {
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& mapping : mappings) {
      if (mapping.id == mapping_id) {
        return mapping;
      }
    }
    return std::nullopt;
  }

  std::pair<std::string, int> ProxyAddressSnapshot() const {
    std::lock_guard<std::mutex> lock(mu);
    return {proxy_host, proxy_port};
  }

  void HandleSyncResult(const json& payload) {
    if (!payload.contains("items") || !payload["items"].is_array()) {
      return;
    }
    for (const auto& item : payload["items"]) {
      if (!item.is_object()) {
        continue;
      }
      const std::string mapping_id = JsonStringValue(item, "mapping_id");
      if (mapping_id.empty()) {
        continue;
      }
      const bool ok = JsonBoolValue(item, "ok", false);
      const std::string error = JsonStringValue(item, "error");
      if (ok) {
        SetRuntime(mapping_id, true, "active", "mapping is active");
      } else {
        SetRuntime(mapping_id, false, "error", error.empty() ? "proxy rejected mapping" : error);
      }
    }
  }

  void HandleTestResult(const json& payload) {
    const std::string request_id = JsonStringValue(payload, "request_id");
    if (request_id.empty()) {
      return;
    }

    std::shared_ptr<PendingTest> target;
    {
      std::lock_guard<std::mutex> lock(tests_mu);
      auto it = pending_tests.find(request_id);
      if (it == pending_tests.end()) {
        return;
      }
      target = it->second;
      pending_tests.erase(it);
    }

    std::lock_guard<std::mutex> lock(target->mu);
    target->done = true;
    target->ok = JsonBoolValue(payload, "ok", false);
    target->detail = JsonStringValue(payload, "detail");
    target->cv.notify_all();
  }

  void HandleUdpFromPeer(const std::string& mapping_id, const std::string& peer, const std::string& payload_b64) {
    const auto mapping = FindMappingById(mapping_id);
    if (!mapping.has_value() || !mapping->enabled || NormalizeProtocol(mapping->protocol) != "udp") {
      return;
    }
    const auto peer_endpoint = ParsePeerEndpoint(peer);
    if (!peer_endpoint.has_value()) {
      return;
    }
    const std::string payload = util::Base64Decode(payload_b64);
    if (payload.empty() && !payload_b64.empty()) {
      return;
    }

    const std::string mapping_id_copy = mapping_id;
    const std::string peer_host = peer_endpoint->first;
    const int peer_port = peer_endpoint->second;
    const std::string local_host = mapping->local_host;
    const int local_port = mapping->local_port;
    std::thread([this, mapping_id_copy, peer_host, peer_port, local_host, local_port, payload]() {
      const auto response = UdpRequestResponse(local_host, local_port, payload, kUdpResponseTimeoutMs);
      if (!response.has_value()) {
        return;
      }
      const std::string response_b64 = util::Base64Encode(*response);
      SendControlJson({
          {"type", "udp_to_peer"},
          {"mapping_id", mapping_id_copy},
          {"peer", BuildPeerEndpoint(peer_host, peer_port)},
          {"payload_b64", response_b64},
      });
    }).detach();
  }

  void HandleTcpWork(const std::string& mapping_id, const std::string& token) {
    const auto mapping = FindMappingById(mapping_id);
    if (!mapping.has_value() || !mapping->enabled || NormalizeProtocol(mapping->protocol) != "tcp") {
      return;
    }

    const auto [proxy_host_snapshot, proxy_port_snapshot] = ProxyAddressSnapshot();
    if (proxy_host_snapshot.empty() || proxy_port_snapshot <= 0) {
      return;
    }

    std::thread([this, mapping = *mapping, token, proxy_host_snapshot, proxy_port_snapshot]() {
      std::string local_error;
      const int local_fd = ConnectTcpWithTimeout(mapping.local_host, mapping.local_port, 1500, &local_error);
      if (local_fd < 0) {
        SetRuntime(mapping.id, false, "error",
                   "failed to connect local target " + mapping.local_host + ":" + std::to_string(mapping.local_port));
        SendControlJson({
            {"type", "work_error"},
            {"token", token},
            {"message", local_error.empty() ? "local connect failed" : local_error},
        });
        return;
      }

      std::string proxy_error;
      const int work_fd = ConnectTcpWithTimeout(proxy_host_snapshot, proxy_port_snapshot, 2500, &proxy_error);
      if (work_fd < 0) {
        CloseSocket(local_fd);
        SetRuntime(mapping.id, false, "error", "failed to connect proxy work endpoint");
        return;
      }

      const json hello = {
          {"type", "work"},
          {"token", token},
      };
      const std::string line = hello.dump() + "\n";
      if (!SendAll(work_fd, line.data(), line.size())) {
        CloseSocket(local_fd);
        CloseSocket(work_fd);
        return;
      }

      std::atomic<bool> stop_flag{false};
      std::thread uplink([&]() {
        BridgeOneWay(local_fd, work_fd, &stop_flag);
      });
      std::thread downlink([&]() {
        BridgeOneWay(work_fd, local_fd, &stop_flag);
      });
      uplink.join();
      downlink.join();
      CloseSocket(local_fd);
      CloseSocket(work_fd);
    }).detach();
  }

  void HandleControlMessage(const std::string& raw_line) {
    const json payload = json::parse(raw_line, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
      return;
    }
    const std::string type = JsonStringValue(payload, "type");
    if (type == "sync_result") {
      HandleSyncResult(payload);
      return;
    }
    if (type == "ping") {
      SendControlJson({
          {"type", "pong"},
          {"ts", util::UtcNowEpochSeconds()},
      });
      return;
    }
    if (type == "open_tcp_work") {
      const std::string mapping_id = JsonStringValue(payload, "mapping_id");
      const std::string token = JsonStringValue(payload, "token");
      if (!mapping_id.empty() && !token.empty()) {
        HandleTcpWork(mapping_id, token);
      }
      return;
    }
    if (type == "udp_from_peer") {
      const std::string mapping_id = JsonStringValue(payload, "mapping_id");
      const std::string peer = JsonStringValue(payload, "peer");
      const std::string payload_b64 = JsonStringValue(payload, "payload_b64");
      if (!mapping_id.empty() && !peer.empty()) {
        HandleUdpFromPeer(mapping_id, peer, payload_b64);
      }
      return;
    }
    if (type == "test_result") {
      HandleTestResult(payload);
      return;
    }
  }

  bool ReceiveControlMessages() {
    if (control_fd < 0) {
      return false;
    }
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(control_fd, &read_fds);
    struct timeval timeout {};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200 * 1000;
    const int selected = ::select(control_fd + 1, &read_fds, nullptr, nullptr, &timeout);
    if (selected < 0) {
      return false;
    }
    if (selected == 0 || !FD_ISSET(control_fd, &read_fds)) {
      return true;
    }

    std::array<char, 8192> buf{};
    const ssize_t n = ::recv(control_fd, buf.data(), buf.size(), 0);
    if (n <= 0) {
      return false;
    }
    control_buffer.append(buf.data(), static_cast<size_t>(n));

    size_t newline = control_buffer.find('\n');
    while (newline != std::string::npos) {
      std::string line = control_buffer.substr(0, newline);
      control_buffer.erase(0, newline + 1);
      line = util::Trim(line);
      if (!line.empty()) {
        HandleControlMessage(line);
      }
      newline = control_buffer.find('\n');
    }
    return true;
  }

  void MarkDisconnectedRuntimes(const std::string& detail) {
    std::vector<std::string> ids;
    {
      std::lock_guard<std::mutex> lock(mu);
      ids.reserve(mappings.size());
      for (const auto& mapping : mappings) {
        if (mapping.enabled) {
          ids.push_back(mapping.id);
        }
      }
    }
    for (const auto& id : ids) {
      SetRuntime(id, false, "disconnected", detail);
    }
  }

  void RunWorker() {
    while (running.load()) {
      std::string host;
      int port = 0;
      bool has_enabled_mapping = false;
      {
        std::lock_guard<std::mutex> lock(mu);
        host = proxy_host;
        port = proxy_port;
        for (const auto& mapping : mappings) {
          if (mapping.enabled) {
            has_enabled_mapping = true;
            break;
          }
        }
      }

      if (host.empty()) {
        MarkDisconnectedRuntimes("proxy host is not configured");
        std::this_thread::sleep_for(std::chrono::milliseconds(kControlSleepWhenIdleMs));
        continue;
      }
      if (!has_enabled_mapping) {
        MarkDisconnectedRuntimes("no enabled mappings");
        std::this_thread::sleep_for(std::chrono::milliseconds(kControlSleepWhenIdleMs));
        continue;
      }

      std::string connect_error;
      control_fd = ConnectTcpWithTimeout(host, port, 2500, &connect_error);
      if (control_fd < 0) {
        MarkDisconnectedRuntimes(connect_error.empty() ? "failed to connect proxy control endpoint" : connect_error);
        std::this_thread::sleep_for(std::chrono::milliseconds(kControlReconnectDelayMs));
        continue;
      }

      control_connected = true;
      control_buffer.clear();
      LogSystem("info", "tunnel.connect", "connected to proxy " + host + ":" + std::to_string(port));

      if (!SendControlJson({
              {"type", "hello"},
              {"client_id", client_id},
              {"version", "1"},
          })) {
        CloseSocket(control_fd);
        control_fd = -1;
        control_connected = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(kControlReconnectDelayMs));
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(mu);
        sync_needed = true;
      }

      auto last_ping_at = std::chrono::steady_clock::now();
      bool alive = true;
      while (running.load() && alive) {
        bool should_sync = false;
        {
          std::lock_guard<std::mutex> lock(mu);
          should_sync = sync_needed;
        }
        if (should_sync && !SendSyncMappings()) {
          alive = false;
          break;
        }

        if (!ReceiveControlMessages()) {
          alive = false;
          break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_ping_at).count() >=
            kControlPingIntervalSeconds) {
          if (!SendControlJson({
                  {"type", "ping"},
                  {"ts", util::UtcNowEpochSeconds()},
              })) {
            alive = false;
            break;
          }
          last_ping_at = now;
        }
      }

      CloseSocket(control_fd);
      control_fd = -1;
      control_connected = false;
      MarkDisconnectedRuntimes("proxy connection lost");
      FailAllPendingTests("proxy connection lost");
      if (running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kControlReconnectDelayMs));
      }
    }
  }

  std::pair<bool, std::string> ProxyMappingTest(const std::string& mapping_id, int timeout_ms) {
    if (!control_connected || control_fd < 0) {
      return {false, "proxy control channel is disconnected"};
    }

    const std::string request_id = util::RandomHex(16);
    auto pending = std::make_shared<PendingTest>();
    {
      std::lock_guard<std::mutex> lock(tests_mu);
      pending_tests[request_id] = pending;
    }

    if (!SendControlJson({
            {"type", "test_mapping"},
            {"request_id", request_id},
            {"mapping_id", mapping_id},
        })) {
      {
        std::lock_guard<std::mutex> lock(tests_mu);
        pending_tests.erase(request_id);
      }
      return {false, "failed to send test request to proxy"};
    }

    std::unique_lock<std::mutex> lock(pending->mu);
    const bool notified = pending->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
      return pending->done;
    });
    if (!notified) {
      std::lock_guard<std::mutex> tests_lock(tests_mu);
      pending_tests.erase(request_id);
      return {false, "proxy test timeout"};
    }
    return {pending->ok, pending->detail.empty() ? (pending->ok ? "ok" : "proxy rejected mapping") : pending->detail};
  }
#endif

  bool TestMapping(const std::string& mapping_id, bool* success, std::string* detail, int timeout_ms) {
    if (success != nullptr) {
      *success = false;
    }
    if (detail != nullptr) {
      detail->clear();
    }

    core::TunnelMappingConfig mapping;
    std::string proxy_host_snapshot;
    int proxy_port_snapshot = 0;
    {
      std::lock_guard<std::mutex> lock(mu);
      proxy_host_snapshot = proxy_host;
      proxy_port_snapshot = proxy_port;
      bool found = false;
      for (const auto& item : mappings) {
        if (item.id == mapping_id) {
          mapping = item;
          found = true;
          break;
        }
      }
      if (!found) {
        if (detail != nullptr) {
          *detail = "mapping not found";
        }
        return false;
      }
    }

    mapping = NormalizeMapping(mapping);
    if (!mapping.enabled) {
      if (detail != nullptr) {
        *detail = "mapping is disabled";
      }
      return true;
    }

#if defined(_WIN32)
    if (detail != nullptr) {
      *detail = "tunnel is not supported on this platform";
    }
    SetRuntime(mapping.id, false, "unsupported", "tunnel is not supported on this platform");
    return false;
#else
    std::ostringstream out;
    bool local_ok = false;
    std::string local_detail;
    if (NormalizeProtocol(mapping.protocol) == "tcp") {
      local_ok = TcpProbe(mapping.local_host, mapping.local_port, 1200, &local_detail);
    } else {
      const auto ports = PortInspector::ListListeningPorts();
      for (const auto& item : ports) {
        if (item.protocol == "udp" && item.port == mapping.local_port) {
          local_ok = true;
          break;
        }
      }
      local_detail = local_ok ? "udp listener exists" : "local udp listener not found";
    }
    out << "local=" << (local_ok ? "ok" : "failed") << " (" << local_detail << ")";

    bool remote_ok = false;
    std::string remote_detail;
    if (proxy_host_snapshot.empty() || proxy_port_snapshot <= 0) {
      remote_ok = false;
      remote_detail = "proxy host/port is not configured";
    } else if (NormalizeProtocol(mapping.protocol) == "tcp") {
      remote_ok = TcpProbe(proxy_host_snapshot, mapping.remote_port, 1800, &remote_detail);
      if (!remote_ok) {
        const auto proxy_test = ProxyMappingTest(mapping.id, timeout_ms);
        if (proxy_test.first) {
          remote_ok = true;
          remote_detail = proxy_test.second;
        }
      }
    } else {
      const auto proxy_test = ProxyMappingTest(mapping.id, timeout_ms);
      remote_ok = proxy_test.first;
      remote_detail = proxy_test.second;
      if (!remote_ok) {
        std::string udp_probe_error;
        if (SendUdpDatagram(proxy_host_snapshot, mapping.remote_port, "ferryman-udp-probe", &udp_probe_error)) {
          remote_detail += "; udp probe datagram sent";
        } else if (!udp_probe_error.empty()) {
          remote_detail += "; " + udp_probe_error;
        }
      }
    }
    out << ", remote=" << (remote_ok ? "ok" : "failed") << " (" << remote_detail << ")";

    const bool final_ok = local_ok && remote_ok;
    if (success != nullptr) {
      *success = final_ok;
    }
    if (detail != nullptr) {
      *detail = out.str();
    }
    SetRuntime(mapping.id, final_ok, final_ok ? "active" : "error", out.str());
    return true;
#endif
  }
};

TunnelManager::TunnelManager(core::AuditLogger* logger) : impl_(std::make_unique<Impl>(logger)) {}

TunnelManager::~TunnelManager() {
  Stop();
}

void TunnelManager::Start() {
  if (impl_->running.exchange(true)) {
    return;
  }
  impl_->worker = std::thread([this]() {
    impl_->RunWorker();
  });
}

void TunnelManager::Stop() {
  if (!impl_->running.exchange(false)) {
    return;
  }
  impl_->FailAllPendingTests("tunnel manager stopped");
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
}

void TunnelManager::Configure(const std::string& proxy_host, int proxy_port,
                              const std::vector<core::TunnelMappingConfig>& mappings) {
  impl_->Configure(proxy_host, proxy_port, mappings);
}

std::string TunnelManager::ProxyHost() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->proxy_host;
}

int TunnelManager::ProxyPort() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->proxy_port;
}

std::vector<core::TunnelMappingConfig> TunnelManager::Mappings() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->mappings;
}

std::vector<TunnelMappingSnapshot> TunnelManager::Snapshot() const {
  std::vector<TunnelMappingSnapshot> snapshots;
  std::lock_guard<std::mutex> lock(impl_->mu);
  snapshots.reserve(impl_->mappings.size());
  for (const auto& mapping : impl_->mappings) {
    TunnelMappingRuntime runtime;
    auto it = impl_->runtime_by_id.find(mapping.id);
    if (it != impl_->runtime_by_id.end()) {
      runtime = it->second;
    } else {
      runtime = TunnelMappingRuntime{
          .active = false,
          .status = "pending",
          .detail = "waiting for status",
          .updated_at = RuntimeNow(),
      };
    }
    snapshots.push_back({
        .mapping = mapping,
        .runtime = runtime,
    });
  }
  return snapshots;
}

bool TunnelManager::TestMapping(const std::string& mapping_id, bool* success, std::string* detail, int timeout_ms) {
  return impl_->TestMapping(mapping_id, success, detail, timeout_ms);
}

}  // namespace ferryman::tunnel
