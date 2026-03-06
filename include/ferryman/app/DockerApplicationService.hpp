#pragma once

#include "ferryman/docker/DockerManager.hpp"

#include <string>
#include <vector>

namespace ferryman::app {

class DockerApplicationService {
 public:
  explicit DockerApplicationService(docker_runtime::DockerManager& docker_manager) : docker_manager_(docker_manager) {}

  std::vector<docker_runtime::ContainerInfo> ListContainers(bool include_all, std::string* error) const {
    return docker_manager_.ListContainers(include_all, error);
  }

  bool StartDockerService(std::string* error) const {
    return docker_manager_.StartDockerService(error);
  }

  bool StartContainer(const std::string& name, std::string* error) const {
    return docker_manager_.StartContainer(name, error);
  }

  bool StopContainer(const std::string& name, std::string* error) const {
    return docker_manager_.StopContainer(name, error);
  }

  bool RestartContainer(const std::string& name, std::string* error) const {
    return docker_manager_.RestartContainer(name, error);
  }

  bool GetLogs(const std::string& name, int tail_lines, std::string* logs, std::string* error) const {
    return docker_manager_.GetLogs(name, tail_lines, logs, error);
  }

  bool InspectContainer(const std::string& name, std::string* inspect, std::string* error) const {
    return docker_manager_.InspectContainer(name, inspect, error);
  }

  bool GetStats(const std::string& name, docker_runtime::ContainerStats* stats, std::string* error) const {
    return docker_manager_.GetStats(name, stats, error);
  }

  bool GetProcesses(const std::string& name, int max_rows, docker_runtime::ContainerProcessSnapshot* processes,
                    std::string* error) const {
    return docker_manager_.GetProcesses(name, max_rows, processes, error);
  }

  bool ListFiles(const std::string& name, const std::string& path,
                 std::vector<docker_runtime::ContainerFileEntry>* entries, std::string* current_path,
                 std::string* error) const {
    return docker_manager_.ListFiles(name, path, entries, current_path, error);
  }

  bool ReadFile(const std::string& name, const std::string& path, std::string* content, std::string* error) const {
    return docker_manager_.ReadFile(name, path, content, error);
  }

  bool WriteFile(const std::string& name, const std::string& path, const std::string& content,
                 std::string* error) const {
    return docker_manager_.WriteFile(name, path, content, error);
  }

 private:
  docker_runtime::DockerManager& docker_manager_;
};

}  // namespace ferryman::app
