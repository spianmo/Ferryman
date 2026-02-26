#pragma once

#include "ferryman/core/ConfigManager.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ferryman::core {
class AuditLogger;
}

namespace ferryman::tunnel {

struct TunnelMappingRuntime {
  bool active = false;
  std::string status;
  std::string detail;
  std::string updated_at;
};

struct TunnelMappingSnapshot {
  core::TunnelMappingConfig mapping;
  TunnelMappingRuntime runtime;
};

class TunnelManager {
 public:
  explicit TunnelManager(core::AuditLogger* logger);
  ~TunnelManager();

  TunnelManager(const TunnelManager&) = delete;
  TunnelManager& operator=(const TunnelManager&) = delete;

  void Start();
  void Stop();

  void Configure(const std::string& proxy_host, int proxy_port, const std::string& proxy_token,
                 const std::vector<core::TunnelMappingConfig>& mappings);

  std::string ProxyHost() const;
  int ProxyPort() const;
  std::vector<core::TunnelMappingConfig> Mappings() const;
  std::vector<TunnelMappingSnapshot> Snapshot() const;

  void SetRuntimeUpdateCallback(std::function<void()> callback);

  bool ProbeProxyEndpoint(const std::string& proxy_host, int proxy_port, const std::string& proxy_token,
                          std::string* detail) const;

  bool TestMapping(const std::string& mapping_id, bool* success, std::string* detail, int timeout_ms = 5000);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ferryman::tunnel
