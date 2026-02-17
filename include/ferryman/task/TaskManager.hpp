#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ferryman::task {

enum class TaskStatus {
  kQueued,
  kRunning,
  kSucceeded,
  kFailed,
};

struct TaskInfo {
  std::string task_id;
  std::string owner_token;
  std::string command;
  TaskStatus status = TaskStatus::kQueued;
  int exit_code = -1;
  std::string created_at;
  std::string updated_at;
  std::string output;
};

class TaskManager {
 public:
  std::string StartTask(const std::string& owner_token, const std::string& command);
  std::optional<TaskInfo> GetTask(const std::string& owner_token, const std::string& task_id);
  std::vector<TaskInfo> ListTasks(const std::string& owner_token);

  static std::string StatusToString(TaskStatus status);

 private:
  void RunTask(const std::string& task_id, const std::string& command);

  std::mutex mu_;
  std::unordered_map<std::string, TaskInfo> tasks_;
};

}  // namespace ferryman::task
