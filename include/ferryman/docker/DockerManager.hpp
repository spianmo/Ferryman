#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ferryman::docker_runtime {

struct ContainerInfo {
  std::string id;
  std::string name;
  std::string image;
  std::string state;
  std::string status;
  std::string running_for;
  std::string ports;
  std::string created_at;
};

struct ContainerStats {
  std::string name;
  double cpu_percent = 0.0;
  uint64_t memory_usage_bytes = 0;
  uint64_t memory_limit_bytes = 0;
  double memory_percent = 0.0;
  uint64_t net_input_bytes = 0;
  uint64_t net_output_bytes = 0;
  uint64_t block_input_bytes = 0;
  uint64_t block_output_bytes = 0;
  int pids = 0;
};

struct ContainerProcessSnapshot {
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
};

struct ContainerFileEntry {
  std::string name;
  std::string path;
  bool is_directory = false;
  uint64_t size = 0;
  int64_t modified_at = 0;
  std::string permissions = "---------";
};

class DockerManager {
 public:
  explicit DockerManager(std::filesystem::path workspace_root);

  std::vector<ContainerInfo> ListContainers(bool include_all, std::string* error) const;
  bool StartContainer(const std::string& name, std::string* error) const;
  bool StopContainer(const std::string& name, std::string* error) const;
  bool RestartContainer(const std::string& name, std::string* error) const;
  bool GetLogs(const std::string& name, int tail_lines, std::string* logs, std::string* error) const;
  bool InspectContainer(const std::string& name, std::string* inspect, std::string* error) const;
  bool GetStats(const std::string& name, ContainerStats* stats, std::string* error) const;
  bool GetProcesses(const std::string& name, int max_rows, ContainerProcessSnapshot* processes,
                    std::string* error) const;

  bool ListFiles(const std::string& name, const std::string& path,
                 std::vector<ContainerFileEntry>* entries, std::string* current_path,
                 std::string* error) const;
  bool ReadFile(const std::string& name, const std::string& path, std::string* content,
                std::string* error) const;
  bool WriteFile(const std::string& name, const std::string& path, const std::string& content,
                 std::string* error) const;

 private:
  std::filesystem::path workspace_root_;
};

}  // namespace ferryman::docker_runtime
