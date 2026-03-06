#pragma once

#include "ferryman/dockurr/DockurrManager.hpp"

#include <string>
#include <vector>

namespace ferryman::app {

class DockurrApplicationService {
 public:
  explicit DockurrApplicationService(dockurr::DockurrManager& dockurr_manager) : dockurr_manager_(dockurr_manager) {}

  std::vector<dockurr::VmInfo> ListVms(std::string* error) const {
    return dockurr_manager_.ListVms(error);
  }

  bool CreateVm(const dockurr::CreateVmRequest& request, dockurr::VmInfo* created_vm, std::string* error) const {
    return dockurr_manager_.CreateVm(request, created_vm, error);
  }

  bool CreateVmWithStartupLogs(const dockurr::CreateVmRequest& request, int max_wait_seconds,
                               const dockurr::DockurrManager::LogCallback& log_callback, dockurr::VmInfo* created_vm,
                               std::string* error) const {
    return dockurr_manager_.CreateVmWithStartupLogs(request, max_wait_seconds, log_callback, created_vm, error);
  }

  bool StartVm(const std::string& name, std::string* error) const {
    return dockurr_manager_.StartVm(name, error);
  }

  bool StopVm(const std::string& name, std::string* error) const {
    return dockurr_manager_.StopVm(name, error);
  }

  bool RestartVm(const std::string& name, std::string* error) const {
    return dockurr_manager_.RestartVm(name, error);
  }

  bool DeleteVm(const std::string& name, std::string* error) const {
    return dockurr_manager_.DeleteVm(name, error);
  }

  bool GetLogs(const std::string& name, int tail_lines, std::string* logs, std::string* error) const {
    return dockurr_manager_.GetLogs(name, tail_lines, logs, error);
  }

  bool InspectVm(const std::string& name, std::string* inspect, std::string* error) const {
    return dockurr_manager_.InspectVm(name, inspect, error);
  }

 private:
  dockurr::DockurrManager& dockurr_manager_;
};

}  // namespace ferryman::app
