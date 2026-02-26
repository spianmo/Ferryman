#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ferryman::dockurr {

struct VmInfo {
  std::string id;
  std::string name;
  std::string os;
  std::string image;
  std::string state;
  bool running = false;
  std::string ports;
  std::string running_for;
  bool persistent = false;
  std::string novnc_port;
  std::string desktop_port;
};

struct CreateVmRequest {
  std::string os;
  std::string version;
  std::string ram_size = "4G";
  std::string disk_size = "64G";
  std::string name;
  bool persistent = false;
};

class DockurrManager {
 public:
  using LogCallback = std::function<void(const std::string&)>;

  explicit DockurrManager(std::filesystem::path workspace_root);

  std::vector<VmInfo> ListVms(std::string* error) const;
  bool CreateVm(const CreateVmRequest& request, VmInfo* created_vm, std::string* error) const;
  bool CreateVmWithStartupLogs(const CreateVmRequest& request, int max_wait_seconds,
                               const LogCallback& log_callback, VmInfo* created_vm,
                               std::string* error) const;
  bool StartVm(const std::string& name, std::string* error) const;
  bool StopVm(const std::string& name, std::string* error) const;
  bool RestartVm(const std::string& name, std::string* error) const;
  bool DeleteVm(const std::string& name, std::string* error) const;
  bool GetLogs(const std::string& name, int tail_lines, std::string* logs, std::string* error) const;
  bool InspectVm(const std::string& name, std::string* inspect, std::string* error) const;
  bool StopTemporaryVms(std::vector<std::string>* stopped_names, std::string* error) const;

 private:
  bool LookupVmByName(const std::string& name, VmInfo* vm, std::string* error) const;

  std::filesystem::path workspace_root_;
};

}  // namespace ferryman::dockurr
