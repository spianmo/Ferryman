#include "ferryman/proxy/ProxyServer.hpp"

#include "ferryman/util/Random.hpp"
#include "ferryman/util/StringUtil.hpp"
#include "ferryman/util/Time.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ferryman::proxy {

namespace {

using nlohmann::json;

constexpr size_t kMaxLogEntries = 8000;
constexpr int kTcpAcceptBacklog = 256;
constexpr int kWorkConnectTimeoutSeconds = 10;

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

int JsonIntValue(const json& payload, const char* key, int fallback = 0) {
  if (!payload.contains(key)) {
    return fallback;
  }
  const auto& value = payload[key];
  if (value.is_number_integer()) {
    return value.get<int>();
  }
  if (value.is_string()) {
    try {
      return std::stoi(value.get<std::string>());
    } catch (...) {
      return fallback;
    }
  }
  return fallback;
}

bool PortValid(int port) {
  return port > 0 && port <= 65535;
}

std::optional<std::pair<std::string, int>> ParseEndpoint(const std::string& text) {
  std::string value = util::Trim(text);
  if (value.empty()) {
    return std::nullopt;
  }
  if (value.front() == '[') {
    const size_t close = value.find(']');
    if (close != std::string::npos && close + 2 <= value.size() && value[close + 1] == ':') {
      int port = 0;
      try {
        port = std::stoi(value.substr(close + 2));
      } catch (...) {
        return std::nullopt;
      }
      if (!PortValid(port)) {
        return std::nullopt;
      }
      return std::make_pair(value.substr(1, close - 1), port);
    }
    return std::nullopt;
  }
  const size_t colon = value.rfind(':');
  if (colon == std::string::npos) {
    return std::nullopt;
  }
  int port = 0;
  try {
    port = std::stoi(value.substr(colon + 1));
  } catch (...) {
    return std::nullopt;
  }
  if (!PortValid(port)) {
    return std::nullopt;
  }
  return std::make_pair(value.substr(0, colon), port);
}

std::string BuildEndpoint(const std::string& host, int port) {
  if (host.find(':') != std::string::npos) {
    return "[" + host + "]:" + std::to_string(port);
  }
  return host + ":" + std::to_string(port);
}

#if defined(__linux__)
void CloseSocket(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

bool SetReuseAddr(int fd) {
  int on = 1;
  return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == 0;
}

bool SendAll(int fd, const char* data, size_t size) {
  size_t sent = 0;
  while (sent < size) {
    const ssize_t n = ::send(fd, data + sent, size - sent, 0);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

int CreateTcpListener(const std::string& host, int port, std::string* error) {
  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo* result = nullptr;
  const std::string port_text = std::to_string(port);
  const int gai = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), port_text.c_str(), &hints, &result);
  if (gai != 0) {
    if (error != nullptr) {
      *error = std::string("getaddrinfo failed: ") + gai_strerror(gai);
    }
    return -1;
  }

  int listener = -1;
  for (struct addrinfo* it = result; it != nullptr; it = it->ai_next) {
    const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) {
      continue;
    }
    SetReuseAddr(fd);
    if (::bind(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen)) != 0) {
      CloseSocket(fd);
      continue;
    }
    if (::listen(fd, kTcpAcceptBacklog) != 0) {
      CloseSocket(fd);
      continue;
    }
    listener = fd;
    break;
  }
  ::freeaddrinfo(result);

  if (listener < 0 && error != nullptr && error->empty()) {
    *error = std::string("failed to bind/listen on ") + host + ":" + std::to_string(port);
  }
  return listener;
}

int CreateUdpListener(const std::string& host, int port, std::string* error) {
  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo* result = nullptr;
  const std::string port_text = std::to_string(port);
  const int gai = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), port_text.c_str(), &hints, &result);
  if (gai != 0) {
    if (error != nullptr) {
      *error = std::string("getaddrinfo failed: ") + gai_strerror(gai);
    }
    return -1;
  }

  int listener = -1;
  for (struct addrinfo* it = result; it != nullptr; it = it->ai_next) {
    const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) {
      continue;
    }
    SetReuseAddr(fd);
    if (::bind(fd, it->ai_addr, static_cast<socklen_t>(it->ai_addrlen)) != 0) {
      CloseSocket(fd);
      continue;
    }
    listener = fd;
    break;
  }
  ::freeaddrinfo(result);

  if (listener < 0 && error != nullptr && error->empty()) {
    *error = std::string("failed to bind udp on ") + host + ":" + std::to_string(port);
  }
  return listener;
}

bool ResolveAddress(const std::string& host, int port, int socktype, int protocol, struct sockaddr_storage* out_addr,
                    socklen_t* out_len) {
  if (out_addr == nullptr || out_len == nullptr) {
    return false;
  }

  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = socktype;
  hints.ai_protocol = protocol;

  struct addrinfo* result = nullptr;
  const std::string port_text = std::to_string(port);
  const int gai = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result);
  if (gai != 0 || result == nullptr) {
    return false;
  }

  std::memset(out_addr, 0, sizeof(*out_addr));
  std::memcpy(out_addr, result->ai_addr, static_cast<size_t>(result->ai_addrlen));
  *out_len = static_cast<socklen_t>(result->ai_addrlen);
  ::freeaddrinfo(result);
  return true;
}

std::string SockAddrToEndpoint(const struct sockaddr* addr, socklen_t addr_len) {
  if (addr == nullptr) {
    return "";
  }
  char host[NI_MAXHOST] = {0};
  char service[NI_MAXSERV] = {0};
  if (::getnameinfo(addr, addr_len, host, sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) !=
      0) {
    return "";
  }
  return BuildEndpoint(host, std::atoi(service));
}

void BridgeOneWay(int from_fd, int to_fd, std::atomic<bool>* stop_flag, std::atomic<uint64_t>* bytes_counter) {
  std::array<char, 16 * 1024> buffer{};
  while (!stop_flag->load()) {
    const ssize_t n = ::recv(from_fd, buffer.data(), buffer.size(), 0);
    if (n <= 0) {
      break;
    }
    if (!SendAll(to_fd, buffer.data(), static_cast<size_t>(n))) {
      break;
    }
    if (bytes_counter != nullptr) {
      bytes_counter->fetch_add(static_cast<uint64_t>(n));
    }
  }
  stop_flag->store(true);
  ::shutdown(to_fd, SHUT_WR);
  ::shutdown(from_fd, SHUT_RD);
}
#endif

}  // namespace

struct ProxyServer::Impl {
  explicit Impl(ProxyServerOptions opts) : options(std::move(opts)) {}

  struct ClientSession {
    std::string id;
    int fd = -1;
    std::string buffer;
    std::mutex send_mu;
    std::thread thread;
    std::atomic<bool> alive{true};
  };

  struct Mapping {
    std::string key;
    std::string client_id;
    std::string mapping_id;
    std::string name;
    std::string protocol;
    std::string local_host;
    int local_port = 0;
    int remote_port = 0;
    int listener_fd = -1;
    std::thread thread;
    std::atomic<bool> active{false};
    std::atomic<uint64_t> ingress_bytes{0};
    std::atomic<uint64_t> egress_bytes{0};
  };

  struct PendingTcpWork {
    int external_fd = -1;
    std::string mapping_key;
    std::chrono::steady_clock::time_point created_at;
  };

  ProxyServerOptions options;

  mutable std::mutex state_mu;
  std::unordered_map<std::string, std::shared_ptr<ClientSession>> clients_by_id;
  std::unordered_map<std::string, std::shared_ptr<Mapping>> mappings_by_key;
  std::unordered_map<std::string, std::string> port_owner;  // protocol:port -> mapping_key
  std::unordered_map<std::string, PendingTcpWork> pending_tcp;

  std::atomic<bool> stop_requested{false};
  int control_listener_fd = -1;
  int admin_listener_fd = -1;
  std::thread control_accept_thread;
  std::thread admin_accept_thread;

  mutable std::mutex log_mu;
  std::deque<std::string> logs;
  std::ofstream log_stream;

  void Log(const std::string& level, const std::string& action, const std::string& detail) {
    const std::string line = "[ferryman-proxy][" + level + "][" + util::UtcNowIso8601() + "][" + action + "] " + detail;
    std::lock_guard<std::mutex> lock(log_mu);
    logs.push_back(line);
    if (logs.size() > kMaxLogEntries) {
      logs.pop_front();
    }
    std::cout << line << '\n';
    if (log_stream.is_open()) {
      log_stream << line << '\n';
      log_stream.flush();
    }
  }

  std::string MappingKey(const std::string& client_id, const std::string& mapping_id) const {
    return client_id + "::" + mapping_id;
  }

  std::string PortKey(const std::string& protocol, int port) const {
    return NormalizeProtocol(protocol) + ":" + std::to_string(port);
  }

  bool TokenAuthorized(const std::string& provided_token) const {
    if (options.auth_token.empty()) {
      return true;
    }
    return provided_token == options.auth_token;
  }

#if defined(__linux__)
  bool SendJson(const std::shared_ptr<ClientSession>& client, const json& payload) {
    if (!client || client->fd < 0) {
      return false;
    }
    const std::string line = payload.dump() + "\n";
    std::lock_guard<std::mutex> lock(client->send_mu);
    return SendAll(client->fd, line.data(), line.size());
  }

  bool SendJsonToClientId(const std::string& client_id, const json& payload) {
    std::shared_ptr<ClientSession> client;
    {
      std::lock_guard<std::mutex> lock(state_mu);
      auto it = clients_by_id.find(client_id);
      if (it == clients_by_id.end()) {
        return false;
      }
      client = it->second;
    }
    return SendJson(client, payload);
  }

  std::shared_ptr<Mapping> FindMappingByClientAndIdLocked(const std::string& client_id, const std::string& mapping_id) const {
    const std::string key = MappingKey(client_id, mapping_id);
    auto it = mappings_by_key.find(key);
    if (it == mappings_by_key.end()) {
      return nullptr;
    }
    return it->second;
  }

  void RemovePendingToken(const std::string& token, const std::string& reason) {
    PendingTcpWork pending;
    bool found = false;
    {
      std::lock_guard<std::mutex> lock(state_mu);
      auto it = pending_tcp.find(token);
      if (it != pending_tcp.end()) {
        pending = it->second;
        pending_tcp.erase(it);
        found = true;
      }
    }
    if (found) {
      CloseSocket(pending.external_fd);
      if (!reason.empty()) {
        Log("warn", "tcp.pending", "closed pending token " + token + ": " + reason);
      }
    }
  }

  void RemoveMapping(const std::string& mapping_key) {
    std::shared_ptr<Mapping> mapping;
    {
      std::lock_guard<std::mutex> lock(state_mu);
      auto it = mappings_by_key.find(mapping_key);
      if (it == mappings_by_key.end()) {
        return;
      }
      mapping = it->second;
      mappings_by_key.erase(it);
      port_owner.erase(PortKey(mapping->protocol, mapping->remote_port));
    }

    if (mapping) {
      mapping->active.store(false);
      CloseSocket(mapping->listener_fd);
      mapping->listener_fd = -1;
      if (mapping->thread.joinable()) {
        mapping->thread.join();
      }
      Log("info", "mapping.remove", mapping_key + " protocol=" + mapping->protocol +
                                        " remote_port=" + std::to_string(mapping->remote_port));
    }
  }

  bool AddOrUpdateMapping(const std::shared_ptr<ClientSession>& client, const json& item, std::string* error) {
    if (!client) {
      if (error != nullptr) {
        *error = "client is null";
      }
      return false;
    }

    const std::string mapping_id = util::Trim(JsonStringValue(item, "id"));
    if (mapping_id.empty()) {
      if (error != nullptr) {
        *error = "mapping id is required";
      }
      return false;
    }
    const std::string protocol = NormalizeProtocol(JsonStringValue(item, "protocol", "tcp"));
    const int remote_port = JsonIntValue(item, "remote_port", 0);
    const int local_port = JsonIntValue(item, "local_port", 0);
    const std::string local_host = util::Trim(JsonStringValue(item, "local_host", "127.0.0.1"));
    const std::string name = JsonStringValue(item, "name", mapping_id);
    const bool enabled = JsonBoolValue(item, "enabled", true);
    if (!enabled) {
      RemoveMapping(MappingKey(client->id, mapping_id));
      return true;
    }

    if (!PortValid(remote_port) || !PortValid(local_port)) {
      if (error != nullptr) {
        *error = "remote/local port is invalid";
      }
      return false;
    }

    const std::string mapping_key = MappingKey(client->id, mapping_id);
    const std::string port_key = PortKey(protocol, remote_port);

    std::shared_ptr<Mapping> existing;
    {
      std::lock_guard<std::mutex> lock(state_mu);
      auto it = mappings_by_key.find(mapping_key);
      if (it != mappings_by_key.end()) {
        existing = it->second;
      }
      auto owner_it = port_owner.find(port_key);
      if (owner_it != port_owner.end() && owner_it->second != mapping_key) {
        if (error != nullptr) {
          *error = "remote port already occupied";
        }
        return false;
      }
    }

    if (existing != nullptr && existing->protocol == protocol && existing->remote_port == remote_port &&
        existing->local_host == local_host && existing->local_port == local_port && existing->active.load()) {
      return true;
    }

    if (existing != nullptr) {
      RemoveMapping(mapping_key);
    }

    std::string listener_error;
    int listener_fd = -1;
    if (protocol == "tcp") {
      listener_fd = CreateTcpListener(options.bind_host, remote_port, &listener_error);
    } else {
      listener_fd = CreateUdpListener(options.bind_host, remote_port, &listener_error);
    }
    if (listener_fd < 0) {
      if (error != nullptr) {
        *error = listener_error.empty() ? "failed to create listener" : listener_error;
      }
      return false;
    }

    auto mapping = std::make_shared<Mapping>();
    mapping->key = mapping_key;
    mapping->client_id = client->id;
    mapping->mapping_id = mapping_id;
    mapping->name = name;
    mapping->protocol = protocol;
    mapping->local_host = local_host.empty() ? "127.0.0.1" : local_host;
    mapping->local_port = local_port;
    mapping->remote_port = remote_port;
    mapping->listener_fd = listener_fd;
    mapping->active.store(true);

    {
      std::lock_guard<std::mutex> lock(state_mu);
      mappings_by_key[mapping_key] = mapping;
      port_owner[port_key] = mapping_key;
    }

    if (protocol == "tcp") {
      mapping->thread = std::thread([this, mapping]() {
        TcpAcceptLoop(mapping);
      });
    } else {
      mapping->thread = std::thread([this, mapping]() {
        UdpLoop(mapping);
      });
    }

    Log("info", "mapping.add", mapping_key + " protocol=" + protocol + " remote_port=" + std::to_string(remote_port) +
                                   " local=" + mapping->local_host + ":" + std::to_string(local_port));
    return true;
  }

  void ApplySyncMappings(const std::shared_ptr<ClientSession>& client, const json& payload_array) {
    if (!client) {
      return;
    }
    if (!payload_array.is_array()) {
      SendJson(client, {
                           {"type", "sync_result"},
                           {"items", json::array({{{"mapping_id", ""}, {"ok", false}, {"error", "mappings must be array"}}})},
                       });
      return;
    }

    std::set<std::string> desired_keys;
    json result_items = json::array();
    for (const auto& item : payload_array) {
      if (!item.is_object()) {
        continue;
      }
      const std::string mapping_id = util::Trim(JsonStringValue(item, "id"));
      if (mapping_id.empty()) {
        continue;
      }
      desired_keys.insert(MappingKey(client->id, mapping_id));
      std::string error;
      const bool ok = AddOrUpdateMapping(client, item, &error);
      result_items.push_back({
          {"mapping_id", mapping_id},
          {"ok", ok},
          {"error", error},
      });
    }

    std::vector<std::string> to_remove;
    {
      std::lock_guard<std::mutex> lock(state_mu);
      for (const auto& [key, mapping] : mappings_by_key) {
        if (mapping->client_id != client->id) {
          continue;
        }
        if (desired_keys.find(key) == desired_keys.end()) {
          to_remove.push_back(key);
        }
      }
    }
    for (const auto& mapping_key : to_remove) {
      RemoveMapping(mapping_key);
    }

    SendJson(client, {
                         {"type", "sync_result"},
                         {"items", result_items},
                     });
  }

  void BridgeTcpConnections(int external_fd, int work_fd, const std::shared_ptr<Mapping>& mapping) {
    if (mapping) {
      mapping->ingress_bytes.fetch_add(0);
      mapping->egress_bytes.fetch_add(0);
    }
    std::atomic<bool> stop{false};
    std::thread t1([&]() {
      BridgeOneWay(external_fd, work_fd, &stop, mapping ? &mapping->ingress_bytes : nullptr);
    });
    std::thread t2([&]() {
      BridgeOneWay(work_fd, external_fd, &stop, mapping ? &mapping->egress_bytes : nullptr);
    });
    t1.join();
    t2.join();
    CloseSocket(external_fd);
    CloseSocket(work_fd);
  }

  void AttachWorkConnection(const std::string& token, int work_fd) {
    PendingTcpWork pending;
    std::shared_ptr<Mapping> mapping;
    {
      std::lock_guard<std::mutex> lock(state_mu);
      auto it = pending_tcp.find(token);
      if (it == pending_tcp.end()) {
        CloseSocket(work_fd);
        return;
      }
      pending = it->second;
      pending_tcp.erase(it);
      auto mapping_it = mappings_by_key.find(pending.mapping_key);
      if (mapping_it != mappings_by_key.end()) {
        mapping = mapping_it->second;
      }
    }

    std::thread([this, pending, work_fd, mapping]() {
      BridgeTcpConnections(pending.external_fd, work_fd, mapping);
    }).detach();
  }

  void TcpAcceptLoop(const std::shared_ptr<Mapping>& mapping) {
    if (!mapping || mapping->listener_fd < 0) {
      return;
    }
    while (!stop_requested.load() && mapping->active.load()) {
      struct sockaddr_storage addr {};
      socklen_t addr_len = sizeof(addr);
      const int external_fd = ::accept(mapping->listener_fd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len);
      if (external_fd < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (stop_requested.load() || !mapping->active.load()) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        continue;
      }

      const std::string token = util::RandomHex(18);
      {
        std::lock_guard<std::mutex> lock(state_mu);
        pending_tcp[token] = PendingTcpWork{
            .external_fd = external_fd,
            .mapping_key = mapping->key,
            .created_at = std::chrono::steady_clock::now(),
        };
      }

      const bool notified = SendJsonToClientId(mapping->client_id,
                                               {
                                                   {"type", "open_tcp_work"},
                                                   {"mapping_id", mapping->mapping_id},
                                                   {"token", token},
                                               });
      if (!notified) {
        RemovePendingToken(token, "client offline");
        continue;
      }

      std::thread([this, token]() {
        std::this_thread::sleep_for(std::chrono::seconds(kWorkConnectTimeoutSeconds));
        RemovePendingToken(token, "work connection timeout");
      }).detach();
    }
  }

  void UdpLoop(const std::shared_ptr<Mapping>& mapping) {
    if (!mapping || mapping->listener_fd < 0) {
      return;
    }
    std::array<char, 65535> buffer{};
    while (!stop_requested.load() && mapping->active.load()) {
      struct sockaddr_storage peer_addr {};
      socklen_t peer_len = sizeof(peer_addr);
      const ssize_t n = ::recvfrom(mapping->listener_fd, buffer.data(), buffer.size(), 0,
                                   reinterpret_cast<struct sockaddr*>(&peer_addr), &peer_len);
      if (n <= 0) {
        if (errno == EINTR) {
          continue;
        }
        if (stop_requested.load() || !mapping->active.load()) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }

      const std::string peer = SockAddrToEndpoint(reinterpret_cast<struct sockaddr*>(&peer_addr), peer_len);
      if (peer.empty()) {
        continue;
      }
      mapping->ingress_bytes.fetch_add(static_cast<uint64_t>(n));
      SendJsonToClientId(mapping->client_id,
                         {
                             {"type", "udp_from_peer"},
                             {"mapping_id", mapping->mapping_id},
                             {"peer", peer},
                             {"payload_b64", util::Base64Encode(std::string(buffer.data(), static_cast<size_t>(n)))},
                         });
    }
  }

  void HandleUdpToPeer(const std::shared_ptr<ClientSession>& client, const json& payload) {
    if (!client) {
      return;
    }
    const std::string mapping_id = JsonStringValue(payload, "mapping_id");
    const std::string peer = JsonStringValue(payload, "peer");
    const std::string payload_b64 = JsonStringValue(payload, "payload_b64");
    if (mapping_id.empty() || peer.empty()) {
      return;
    }

    std::shared_ptr<Mapping> mapping;
    {
      std::lock_guard<std::mutex> lock(state_mu);
      mapping = FindMappingByClientAndIdLocked(client->id, mapping_id);
    }
    if (!mapping || NormalizeProtocol(mapping->protocol) != "udp" || mapping->listener_fd < 0) {
      return;
    }

    const auto endpoint = ParseEndpoint(peer);
    if (!endpoint.has_value()) {
      return;
    }
    const std::string bytes = util::Base64Decode(payload_b64);
    if (bytes.empty() && !payload_b64.empty()) {
      return;
    }

    struct sockaddr_storage addr {};
    socklen_t addr_len = 0;
    if (!ResolveAddress(endpoint->first, endpoint->second, SOCK_DGRAM, IPPROTO_UDP, &addr, &addr_len)) {
      return;
    }
    const ssize_t sent =
        ::sendto(mapping->listener_fd, bytes.data(), bytes.size(), 0, reinterpret_cast<struct sockaddr*>(&addr), addr_len);
    if (sent > 0) {
      mapping->egress_bytes.fetch_add(static_cast<uint64_t>(sent));
    }
  }

  void HandleTestMapping(const std::shared_ptr<ClientSession>& client, const json& payload) {
    if (!client) {
      return;
    }
    const std::string request_id = JsonStringValue(payload, "request_id");
    const std::string mapping_id = JsonStringValue(payload, "mapping_id");
    bool ok = false;
    std::string detail = "mapping not found";
    if (!mapping_id.empty()) {
      std::lock_guard<std::mutex> lock(state_mu);
      auto mapping = FindMappingByClientAndIdLocked(client->id, mapping_id);
      if (mapping && mapping->active.load() && mapping->listener_fd >= 0) {
        ok = true;
        detail = "proxy listener active";
      }
    }
    SendJson(client, {
                         {"type", "test_result"},
                         {"request_id", request_id},
                         {"mapping_id", mapping_id},
                         {"ok", ok},
                         {"detail", detail},
                     });
  }

  void HandleClientMessage(const std::shared_ptr<ClientSession>& client, const std::string& line) {
    const json payload = json::parse(line, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
      return;
    }
    const std::string type = JsonStringValue(payload, "type");
    if (type == "sync_mappings") {
      ApplySyncMappings(client, payload.value("mappings", json::array()));
      return;
    }
    if (type == "udp_to_peer") {
      HandleUdpToPeer(client, payload);
      return;
    }
    if (type == "test_mapping") {
      HandleTestMapping(client, payload);
      return;
    }
    if (type == "work_error") {
      const std::string token = JsonStringValue(payload, "token");
      const std::string message = JsonStringValue(payload, "message");
      if (!token.empty()) {
        RemovePendingToken(token, message);
      }
      return;
    }
    if (type == "ping") {
      SendJson(client, {
                           {"type", "pong"},
                           {"ts", util::UtcNowEpochSeconds()},
                       });
      return;
    }
  }

  void RemoveClientMappings(const std::string& client_id) {
    std::vector<std::string> keys;
    {
      std::lock_guard<std::mutex> lock(state_mu);
      for (const auto& [key, mapping] : mappings_by_key) {
        if (mapping->client_id == client_id) {
          keys.push_back(key);
        }
      }
    }
    for (const auto& key : keys) {
      RemoveMapping(key);
    }
  }

  void HandleClientDisconnect(const std::shared_ptr<ClientSession>& client) {
    if (!client) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(state_mu);
      clients_by_id.erase(client->id);
    }
    RemoveClientMappings(client->id);
    Log("info", "client.disconnect", client->id);
  }

  void ClientLoop(const std::shared_ptr<ClientSession>& client) {
    while (!stop_requested.load() && client->alive.load()) {
      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(client->fd, &read_fds);
      struct timeval timeout {};
      timeout.tv_sec = 0;
      timeout.tv_usec = 250 * 1000;
      const int selected = ::select(client->fd + 1, &read_fds, nullptr, nullptr, &timeout);
      if (selected < 0) {
        break;
      }
      if (selected == 0 || !FD_ISSET(client->fd, &read_fds)) {
        continue;
      }

      std::array<char, 8192> buf{};
      const ssize_t n = ::recv(client->fd, buf.data(), buf.size(), 0);
      if (n <= 0) {
        break;
      }
      client->buffer.append(buf.data(), static_cast<size_t>(n));
      size_t pos = client->buffer.find('\n');
      while (pos != std::string::npos) {
        std::string line = util::Trim(client->buffer.substr(0, pos));
        client->buffer.erase(0, pos + 1);
        if (!line.empty()) {
          HandleClientMessage(client, line);
        }
        pos = client->buffer.find('\n');
      }
    }

    client->alive.store(false);
    CloseSocket(client->fd);
    client->fd = -1;
    HandleClientDisconnect(client);
  }

  std::optional<std::string> ReadJsonLineOnce(int fd) {
    std::string line;
    std::array<char, 1> ch{};
    for (size_t i = 0; i < 4096; ++i) {
      const ssize_t n = ::recv(fd, ch.data(), 1, 0);
      if (n <= 0) {
        return std::nullopt;
      }
      if (ch[0] == '\n') {
        break;
      }
      line.push_back(ch[0]);
    }
    line = util::Trim(line);
    if (line.empty()) {
      return std::nullopt;
    }
    return line;
  }

  void HandleAcceptedControlConnection(int fd) {
    const auto first_line = ReadJsonLineOnce(fd);
    if (!first_line.has_value()) {
      CloseSocket(fd);
      return;
    }
    const json first_payload = json::parse(*first_line, nullptr, false);
    if (first_payload.is_discarded() || !first_payload.is_object()) {
      CloseSocket(fd);
      return;
    }

    const std::string type = JsonStringValue(first_payload, "type");
    const std::string auth_token = JsonStringValue(first_payload, "auth_token");
    if (!TokenAuthorized(auth_token)) {
      CloseSocket(fd);
      Log("warn", "auth.reject", "unauthorized control handshake");
      return;
    }
    if (type == "work") {
      const std::string token = JsonStringValue(first_payload, "token");
      if (token.empty()) {
        CloseSocket(fd);
        return;
      }
      AttachWorkConnection(token, fd);
      return;
    }
    if (type != "hello") {
      CloseSocket(fd);
      return;
    }

    std::string client_id = util::Trim(JsonStringValue(first_payload, "client_id"));
    if (client_id.empty()) {
      client_id = "client-" + util::RandomHex(10);
    }

    auto client = std::make_shared<ClientSession>();
    client->id = client_id;
    client->fd = fd;
    client->alive.store(true);

    {
      std::lock_guard<std::mutex> lock(state_mu);
      auto existing = clients_by_id.find(client_id);
      if (existing != clients_by_id.end()) {
        existing->second->alive.store(false);
        CloseSocket(existing->second->fd);
      }
      clients_by_id[client_id] = client;
    }

    SendJson(client, {
                         {"type", "hello_ack"},
                         {"ok", true},
                         {"server_ts", util::UtcNowIso8601()},
                     });
    Log("info", "client.connect", client_id);

    client->thread = std::thread([this, client]() {
      ClientLoop(client);
    });
    client->thread.detach();
  }

  void ControlAcceptLoop() {
    while (!stop_requested.load()) {
      struct sockaddr_storage addr {};
      socklen_t addr_len = sizeof(addr);
      const int fd = ::accept(control_listener_fd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len);
      if (fd < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (stop_requested.load()) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      std::thread([this, fd]() {
        HandleAcceptedControlConnection(fd);
      }).detach();
    }
  }

  void HandleAdminCommand(int fd) {
    std::string raw;
    std::array<char, 4096> buf{};
    while (raw.find('\n') == std::string::npos && raw.size() < 4096) {
      const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
      if (n <= 0) {
        break;
      }
      raw.append(buf.data(), static_cast<size_t>(n));
    }
    if (raw.empty()) {
      CloseSocket(fd);
      return;
    }

    std::string command;
    {
      std::istringstream stream(raw);
      std::getline(stream, command);
      command = util::Trim(command);
    }

    if (!options.auth_token.empty()) {
      if (command.rfind("TOKEN ", 0) != 0) {
        const std::string unauthorized = json({{"ok", false}, {"error", "unauthorized"}}).dump() + "\n";
        SendAll(fd, unauthorized.data(), unauthorized.size());
        CloseSocket(fd);
        return;
      }
      std::string auth_suffix = util::Trim(command.substr(6));
      size_t split = auth_suffix.find_first_of(" \t");
      std::string provided_token = auth_suffix;
      command.clear();
      if (split != std::string::npos) {
        provided_token = util::Trim(auth_suffix.substr(0, split));
        command = util::Trim(auth_suffix.substr(split + 1));
      }
      if (!TokenAuthorized(provided_token)) {
        const std::string unauthorized = json({{"ok", false}, {"error", "unauthorized"}}).dump() + "\n";
        SendAll(fd, unauthorized.data(), unauthorized.size());
        CloseSocket(fd);
        Log("warn", "auth.reject", "unauthorized admin command");
        return;
      }
    }
    command = util::Trim(command);

    std::string response;
    if (command.empty() || command == "LIST") {
      response = DumpMappingsJson();
    } else if (command == "STATUS") {
      response = DumpStatusJson();
    } else if (command.rfind("LOGS", 0) == 0) {
      size_t limit = 100;
      const std::string suffix = util::Trim(command.substr(4));
      if (!suffix.empty()) {
        try {
          limit = static_cast<size_t>(std::stoul(suffix));
        } catch (...) {
          limit = 100;
        }
      }
      response = TailLogsJson(limit);
    } else {
      response = json({{"ok", false}, {"error", "unknown command"}}).dump();
    }
    response.push_back('\n');
    SendAll(fd, response.data(), response.size());
    CloseSocket(fd);
  }

  void AdminAcceptLoop() {
    while (!stop_requested.load()) {
      struct sockaddr_storage addr {};
      socklen_t addr_len = sizeof(addr);
      const int fd = ::accept(admin_listener_fd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len);
      if (fd < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (stop_requested.load()) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      std::thread([this, fd]() {
        HandleAdminCommand(fd);
      }).detach();
    }
  }

  bool Start(std::string* error) {
    if (!options.log_file.empty()) {
      std::error_code ec;
      const auto parent = options.log_file.parent_path();
      if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
      }
      log_stream.open(options.log_file, std::ios::app);
      if (!log_stream.is_open() && error != nullptr) {
        *error = "failed to open log file: " + options.log_file.string();
      }
    }

    std::string control_error;
    control_listener_fd = CreateTcpListener(options.bind_host, options.control_port, &control_error);
    if (control_listener_fd < 0) {
      if (error != nullptr) {
        *error = control_error;
      }
      return false;
    }

    std::string admin_error;
    admin_listener_fd = CreateTcpListener(options.admin_host, options.admin_port, &admin_error);
    if (admin_listener_fd < 0) {
      if (error != nullptr) {
        *error = admin_error;
      }
      CloseSocket(control_listener_fd);
      control_listener_fd = -1;
      return false;
    }

    stop_requested.store(false);
    control_accept_thread = std::thread([this]() {
      ControlAcceptLoop();
    });
    admin_accept_thread = std::thread([this]() {
      AdminAcceptLoop();
    });

    Log("info", "server.start", "control=" + options.bind_host + ":" + std::to_string(options.control_port) +
                                    ", admin=" + options.admin_host + ":" + std::to_string(options.admin_port));
    return true;
  }

  void Stop() {
    stop_requested.store(true);
    CloseSocket(control_listener_fd);
    CloseSocket(admin_listener_fd);
    control_listener_fd = -1;
    admin_listener_fd = -1;

    std::vector<std::string> mapping_keys;
    std::vector<std::shared_ptr<ClientSession>> clients;
    {
      std::lock_guard<std::mutex> lock(state_mu);
      for (const auto& [key, _] : mappings_by_key) {
        mapping_keys.push_back(key);
      }
      for (const auto& [_, client] : clients_by_id) {
        clients.push_back(client);
      }
      for (auto& [_, pending] : pending_tcp) {
        CloseSocket(pending.external_fd);
      }
      pending_tcp.clear();
      clients_by_id.clear();
    }

    for (const auto& key : mapping_keys) {
      RemoveMapping(key);
    }
    for (const auto& client : clients) {
      if (!client) {
        continue;
      }
      client->alive.store(false);
      CloseSocket(client->fd);
    }

    if (control_accept_thread.joinable()) {
      control_accept_thread.join();
    }
    if (admin_accept_thread.joinable()) {
      admin_accept_thread.join();
    }

    Log("info", "server.stop", "proxy shutdown complete");
  }
#endif

  std::string DumpMappingsJson() const {
    json mappings = json::array();
#if defined(__linux__)
    std::lock_guard<std::mutex> lock(state_mu);
    for (const auto& [_, mapping] : mappings_by_key) {
      mappings.push_back({
          {"client_id", mapping->client_id},
          {"mapping_id", mapping->mapping_id},
          {"name", mapping->name},
          {"protocol", mapping->protocol},
          {"remote_port", mapping->remote_port},
          {"local_host", mapping->local_host},
          {"local_port", mapping->local_port},
          {"active", mapping->active.load()},
          {"ingress_bytes", mapping->ingress_bytes.load()},
          {"egress_bytes", mapping->egress_bytes.load()},
      });
    }
#endif
    return json({
                    {"ok", true},
                    {"count", mappings.size()},
                    {"mappings", mappings},
                })
        .dump();
  }

  std::string DumpStatusJson() const {
#if defined(__linux__)
    size_t clients = 0;
    size_t mappings = 0;
    size_t pending = 0;
    {
      std::lock_guard<std::mutex> lock(state_mu);
      clients = clients_by_id.size();
      mappings = mappings_by_key.size();
      pending = pending_tcp.size();
    }
    return json({
                    {"ok", true},
                    {"running", !stop_requested.load()},
                    {"clients", clients},
                    {"mappings", mappings},
                    {"pending_tcp", pending},
                })
        .dump();
#else
    return json({
                    {"ok", false},
                    {"running", false},
                    {"error", "FerrymanProxy is only supported on Linux"},
                })
        .dump();
#endif
  }

  std::string TailLogsJson(size_t limit) const {
    if (limit == 0) {
      limit = 1;
    }
    json items = json::array();
    {
      std::lock_guard<std::mutex> lock(log_mu);
      const size_t start = logs.size() > limit ? logs.size() - limit : 0;
      for (size_t i = start; i < logs.size(); ++i) {
        items.push_back(logs[i]);
      }
    }
    return json({
                    {"ok", true},
                    {"count", items.size()},
                    {"items", items},
                })
        .dump();
  }
};

ProxyServer::ProxyServer(ProxyServerOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

ProxyServer::~ProxyServer() {
  Stop();
}

bool ProxyServer::Start(std::string* error) {
#if !defined(__linux__)
  if (error != nullptr) {
    *error = "FerrymanProxy is only supported on Linux";
  }
  return false;
#else
  if (running_.exchange(true)) {
    return true;
  }
  if (!impl_->Start(error)) {
    running_.store(false);
    return false;
  }
  return true;
#endif
}

void ProxyServer::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  impl_->Stop();
}

std::string ProxyServer::DumpMappingsJson() const {
  return impl_->DumpMappingsJson();
}

std::string ProxyServer::DumpStatusJson() const {
  return impl_->DumpStatusJson();
}

std::string ProxyServer::TailLogsJson(size_t limit) const {
  return impl_->TailLogsJson(limit);
}

}  // namespace ferryman::proxy
