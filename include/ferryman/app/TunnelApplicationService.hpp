#pragma once

#include "ferryman/tunnel/TunnelManager.hpp"

#include <string>
#include <vector>

namespace ferryman::app {

class TunnelApplicationService {
 public:
  explicit TunnelApplicationService(tunnel::TunnelManager& tunnel_manager) : tunnel_manager_(tunnel_manager) {}

  std::vector<tunnel::TunnelMappingSnapshot> Snapshot() const {
    return tunnel_manager_.Snapshot();
  }

  bool ProbeProxyEndpoint(const std::string& proxy_host, int proxy_port, const std::string& proxy_token,
                          std::string* detail) const {
    return tunnel_manager_.ProbeProxyEndpoint(proxy_host, proxy_port, proxy_token, detail);
  }

  void Configure(const std::string& proxy_host, int proxy_port, const std::string& proxy_token,
                 const std::vector<core::TunnelMappingConfig>& mappings) {
    tunnel_manager_.Configure(proxy_host, proxy_port, proxy_token, mappings);
  }

  bool TestMapping(const std::string& mapping_id, bool* success, std::string* detail, int timeout_ms = 5000) {
    return tunnel_manager_.TestMapping(mapping_id, success, detail, timeout_ms);
  }

 private:
  tunnel::TunnelManager& tunnel_manager_;
};

}  // namespace ferryman::app
