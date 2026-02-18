#include "ferryman/task/TaskManager.hpp"

#include "ferryman/util/Random.hpp"
#include "ferryman/util/Time.hpp"

#include <array>
#include <cstdio>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#endif

namespace ferryman::task {

namespace {

FILE* OpenPipe(const std::string& command) {
#if defined(_WIN32)
  return ::_popen(command.c_str(), "r");
#else
  return ::popen(command.c_str(), "r");
#endif
}

int ClosePipe(FILE* pipe) {
#if defined(_WIN32)
  return ::_pclose(pipe);
#else
  return ::pclose(pipe);
#endif
}

}  // namespace

std::string TaskManager::StatusToString(TaskStatus status) {
  switch (status) {
    case TaskStatus::kQueued:
      return "queued";
    case TaskStatus::kRunning:
      return "running";
    case TaskStatus::kSucceeded:
      return "succeeded";
    case TaskStatus::kFailed:
      return "failed";
  }
  return "unknown";
}

std::string TaskManager::StartTask(const std::string& owner_token, const std::string& command) {
  TaskInfo info;
  info.task_id = util::RandomHex(20);
  info.owner_token = owner_token;
  info.command = command;
  info.status = TaskStatus::kQueued;
  info.created_at = util::UtcNowIso8601();
  info.updated_at = info.created_at;

  {
    std::lock_guard<std::mutex> lock(mu_);
    tasks_[info.task_id] = info;
  }

  std::thread([this, id = info.task_id, command]() {
    RunTask(id, command);
  }).detach();

  return info.task_id;
}

void TaskManager::RunTask(const std::string& task_id, const std::string& command) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
      return;
    }
    it->second.status = TaskStatus::kRunning;
    it->second.updated_at = util::UtcNowIso8601();
  }

  const std::string wrapped = command + " 2>&1";
  FILE* pipe = OpenPipe(wrapped);
  if (pipe == nullptr) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = tasks_.find(task_id);
    if (it != tasks_.end()) {
      it->second.status = TaskStatus::kFailed;
      it->second.exit_code = -1;
      it->second.output = "failed to launch process";
      it->second.updated_at = util::UtcNowIso8601();
    }
    return;
  }

  std::array<char, 2048> buffer{};
  std::string output;
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output.append(buffer.data());
    std::lock_guard<std::mutex> lock(mu_);
    auto it = tasks_.find(task_id);
    if (it != tasks_.end()) {
      it->second.output = output;
      it->second.updated_at = util::UtcNowIso8601();
    }
  }

  int status_code = ClosePipe(pipe);
  int exit_code = status_code;
#if defined(__unix__) || defined(__APPLE__)
  if (WIFEXITED(status_code)) {
    exit_code = WEXITSTATUS(status_code);
  }
#endif

  std::lock_guard<std::mutex> lock(mu_);
  auto it = tasks_.find(task_id);
  if (it != tasks_.end()) {
    it->second.output = output;
    it->second.exit_code = exit_code;
    it->second.status = exit_code == 0 ? TaskStatus::kSucceeded : TaskStatus::kFailed;
    it->second.updated_at = util::UtcNowIso8601();
  }
}

std::optional<TaskInfo> TaskManager::GetTask(const std::string& owner_token, const std::string& task_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = tasks_.find(task_id);
  if (it == tasks_.end() || it->second.owner_token != owner_token) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<TaskInfo> TaskManager::ListTasks(const std::string& owner_token) {
  std::vector<TaskInfo> list;
  std::lock_guard<std::mutex> lock(mu_);
  list.reserve(tasks_.size());
  for (const auto& [_, task] : tasks_) {
    if (task.owner_token == owner_token) {
      list.push_back(task);
    }
  }
  return list;
}

}  // namespace ferryman::task
