#pragma once

#include "ferryman/task/TaskManager.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ferryman::app {

class TaskApplicationService {
 public:
  explicit TaskApplicationService(task::TaskManager& task_manager) : task_manager_(task_manager) {}

  std::string StartTask(const std::string& owner_token, const std::string& command) {
    return task_manager_.StartTask(owner_token, command);
  }

  std::optional<task::TaskInfo> GetTask(const std::string& owner_token, const std::string& task_id) {
    return task_manager_.GetTask(owner_token, task_id);
  }

  std::vector<task::TaskInfo> ListTasks(const std::string& owner_token) {
    return task_manager_.ListTasks(owner_token);
  }

 private:
  task::TaskManager& task_manager_;
};

}  // namespace ferryman::app
