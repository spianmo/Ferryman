#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ferryman::core {

struct FileEntry {
  std::string name;
  std::string path;
  bool is_directory = false;
  uintmax_t size = 0;
  int64_t modified_at = 0;
};

class FileService {
 public:
  explicit FileService(std::filesystem::path root_path);

  std::optional<std::filesystem::path> ResolvePath(const std::string& request_path) const;
  std::vector<FileEntry> ListDirectory(const std::string& request_path, std::string* error) const;
  std::optional<std::string> ReadFile(const std::string& request_path, std::string* error) const;
  bool WriteFile(const std::string& request_path, const std::string& content, std::string* error) const;

 private:
  std::filesystem::path root_path_;
};

}  // namespace ferryman::core
