#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

namespace ferryman::proxy {

struct ProxyServerOptions {
  std::string bind_host = "0.0.0.0";
  int control_port = 17000;
  std::string admin_host = "127.0.0.1";
  int admin_port = 17001;
  std::string auth_token;
  std::filesystem::path log_file;
};

class ProxyServer {
 public:
  explicit ProxyServer(ProxyServerOptions options);
  ~ProxyServer();

  ProxyServer(const ProxyServer&) = delete;
  ProxyServer& operator=(const ProxyServer&) = delete;

  bool Start(std::string* error);
  void Stop();
  bool running() const { return running_.load(); }

  std::string DumpMappingsJson() const;
  std::string DumpStatusJson() const;
  std::string TailLogsJson(size_t limit) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<bool> running_{false};
};

}  // namespace ferryman::proxy
