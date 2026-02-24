#include "ferryman/core/AuditLogger.hpp"

#include "ferryman/util/StringUtil.hpp"
#include "ferryman/util/Time.hpp"

#include <iostream>
#include <sstream>
#include <utility>

namespace ferryman::core {

namespace {

std::string BuildEntry(const std::string& level, const std::string& session_token,
                       const std::string& action, const std::string& detail) {
  return util::BuildJsonObject({
      {"ts", util::UtcNowIso8601(), false},
      {"level", level, false},
      {"session", session_token, false},
      {"action", action, false},
      {"detail", detail, false},
  });
}

void PrintImmediate(const std::string& level, const std::string& session_token,
                    const std::string& action, const std::string& detail) {
  // Immediate console output: use std::cerr (unbuffered by default).
  std::cerr << "[ferryman]"
            << "[" << level << "]"
            << "[" << util::UtcNowIso8601() << "]"
            << "[session=" << (session_token.empty() ? "-" : session_token.substr(0, 10)) << "]"
            << "[" << action << "] "
            << detail << '\n';
}

}  // namespace

AuditLogger::AuditLogger(std::filesystem::path log_path) : log_path_(std::move(log_path)) {
  (void)log_path_;
}

void AuditLogger::PushEntry(const std::string& serialized) {
  if (entries_.size() >= max_entries_) {
    entries_.pop_front();
  }
  entries_.push_back(serialized);
}

bool AuditLogger::Append(const std::string& session_token, const std::string& action,
                         const std::string& detail) {
  return AppendWithLevel("info", session_token, action, detail);
}

bool AuditLogger::AppendWithLevel(const std::string& level, const std::string& session_token,
                                  const std::string& action, const std::string& detail) {
  const std::string normalized_level = level.empty() ? "info" : level;
  const std::string serialized = BuildEntry(normalized_level, session_token, action, detail);
  PrintImmediate(normalized_level, session_token, action, detail);

  RealtimeCallback callback;
  {
    std::lock_guard<std::mutex> lock(mu_);
    PushEntry(serialized);
    callback = realtime_callback_;
  }
  if (callback) {
    callback(serialized);
  }
  return true;
}

bool AuditLogger::AppendSystem(const std::string& level, const std::string& action,
                               const std::string& detail) {
  return AppendWithLevel(level, "", action, detail);
}

std::string AuditLogger::Tail(size_t max_lines) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (entries_.empty()) {
    return "[]";
  }

  const size_t begin = entries_.size() > max_lines ? entries_.size() - max_lines : 0;
  std::ostringstream out;
  out << '[';
  for (size_t i = begin; i < entries_.size(); ++i) {
    out << entries_[i];
    if (i + 1 < entries_.size()) {
      out << ',';
    }
  }
  out << ']';
  return out.str();
}

void AuditLogger::SetRealtimeCallback(RealtimeCallback callback) {
  std::lock_guard<std::mutex> lock(mu_);
  realtime_callback_ = std::move(callback);
}

}  // namespace ferryman::core
