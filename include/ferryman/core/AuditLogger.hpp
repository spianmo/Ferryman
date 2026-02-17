#pragma once

#include <filesystem>
#include <deque>
#include <mutex>
#include <string>

namespace ferryman::core {

class AuditLogger {
 public:
  explicit AuditLogger(std::filesystem::path log_path);

  bool Append(const std::string& session_token, const std::string& action, const std::string& detail);
  bool AppendSystem(const std::string& level, const std::string& action, const std::string& detail);
  std::string Tail(size_t max_lines) const;

 private:
  void PushEntry(const std::string& serialized);

  std::filesystem::path log_path_;
  mutable std::mutex mu_;
  std::deque<std::string> entries_;
  size_t max_entries_ = 5000;
};

}  // namespace ferryman::core
