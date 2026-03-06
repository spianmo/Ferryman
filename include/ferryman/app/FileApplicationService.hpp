#pragma once

#include "ferryman/core/FileService.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ferryman::app {

class FileApplicationService {
 public:
  explicit FileApplicationService(core::FileService& file_service) : file_service_(file_service) {}

  std::optional<std::filesystem::path> ResolvePath(const std::string& request_path) const {
    return file_service_.ResolvePath(request_path);
  }

  std::vector<core::FileEntry> ListDirectory(const std::string& request_path, std::string* error) const {
    return file_service_.ListDirectory(request_path, error);
  }

  std::optional<std::string> ReadFile(const std::string& request_path, std::string* error) const {
    return file_service_.ReadFile(request_path, error);
  }

  bool WriteFile(const std::string& request_path, const std::string& content, std::string* error) const {
    return file_service_.WriteFile(request_path, content, error);
  }

 private:
  core::FileService& file_service_;
};

}  // namespace ferryman::app
