#pragma once

#include <filesystem>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

namespace ferryman::core {

class AuditLogger {
 public:
  using RealtimeCallback = std::function<void(const std::string& serialized_entry)>;

  explicit AuditLogger(std::filesystem::path log_path);

  bool AppendWithLevel(const std::string& level, const std::string& session_token,
                       const std::string& action, const std::string& detail);
  bool Append(const std::string& session_token, const std::string& action, const std::string& detail);
  bool AppendSystem(const std::string& level, const std::string& action, const std::string& detail);
  std::string Tail(size_t max_lines) const;
  void SetRealtimeCallback(RealtimeCallback callback);

 private:
  void PushEntry(const std::string& serialized);

  std::filesystem::path log_path_;
  mutable std::mutex mu_;
  std::deque<std::string> entries_;
  RealtimeCallback realtime_callback_;
  size_t max_entries_ = 5000;
};

}  // namespace ferryman::core
