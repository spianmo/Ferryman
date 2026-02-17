#include "ferryman/core/FileService.hpp"

#include <algorithm>
#include <fstream>

namespace ferryman::core {

namespace {

int64_t FileTimeToEpoch(std::filesystem::file_time_type time) {
  using namespace std::chrono;
  const auto system_now = time_point_cast<system_clock::duration>(
      time - std::filesystem::file_time_type::clock::now() + system_clock::now());
  return duration_cast<seconds>(system_now.time_since_epoch()).count();
}

bool IsSubPath(const std::filesystem::path& root, const std::filesystem::path& candidate) {
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  for (; root_it != root.end(); ++root_it, ++candidate_it) {
    if (candidate_it == candidate.end() || *root_it != *candidate_it) {
      return false;
    }
  }
  return true;
}

}  // namespace

FileService::FileService(std::filesystem::path root_path) {
  std::error_code ec;
  root_path_ = std::filesystem::weakly_canonical(root_path, ec);
  if (ec) {
    root_path_ = std::filesystem::absolute(root_path);
  }
}

std::optional<std::filesystem::path> FileService::ResolvePath(const std::string& request_path) const {
  if (request_path.empty() || request_path == "/") {
    return root_path_;
  }

  std::filesystem::path raw = request_path;
  if (raw.is_relative()) {
    raw = root_path_ / raw;
  }

  std::error_code ec;
  std::filesystem::path resolved = std::filesystem::weakly_canonical(raw, ec);
  if (ec) {
    return std::nullopt;
  }
  if (!IsSubPath(root_path_, resolved)) {
    return std::nullopt;
  }
  return resolved;
}

std::vector<FileEntry> FileService::ListDirectory(const std::string& request_path, std::string* error) const {
  std::vector<FileEntry> entries;
  auto path = ResolvePath(request_path);
  if (!path.has_value()) {
    if (error != nullptr) {
      *error = "path not allowed";
    }
    return entries;
  }

  std::error_code ec;
  if (!std::filesystem::exists(*path, ec)) {
    if (error != nullptr) {
      *error = "path does not exist";
    }
    return entries;
  }
  if (!std::filesystem::is_directory(*path, ec)) {
    if (error != nullptr) {
      *error = "path is not a directory";
    }
    return entries;
  }

  for (const auto& item : std::filesystem::directory_iterator(*path, ec)) {
    if (ec) {
      break;
    }
    FileEntry entry;
    entry.name = item.path().filename().string();
    entry.path = item.path().string();
    entry.is_directory = item.is_directory(ec);
    if (!entry.is_directory) {
      entry.size = item.file_size(ec);
    }
    entry.modified_at = FileTimeToEpoch(item.last_write_time(ec));
    entries.push_back(std::move(entry));
  }

  std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
    if (a.is_directory != b.is_directory) {
      return a.is_directory > b.is_directory;
    }
    return a.name < b.name;
  });
  return entries;
}

std::optional<std::string> FileService::ReadFile(const std::string& request_path, std::string* error) const {
  auto path = ResolvePath(request_path);
  if (!path.has_value()) {
    if (error != nullptr) {
      *error = "path not allowed";
    }
    return std::nullopt;
  }

  std::error_code ec;
  if (!std::filesystem::exists(*path, ec) || std::filesystem::is_directory(*path, ec)) {
    if (error != nullptr) {
      *error = "file does not exist";
    }
    return std::nullopt;
  }

  std::ifstream file(*path, std::ios::binary);
  if (!file.is_open()) {
    if (error != nullptr) {
      *error = "unable to open file";
    }
    return std::nullopt;
  }

  std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (content.size() > 1024 * 1024 * 8) {
    if (error != nullptr) {
      *error = "file too large (max 8MB)";
    }
    return std::nullopt;
  }
  return content;
}

bool FileService::WriteFile(const std::string& request_path, const std::string& content,
                            std::string* error) const {
  auto path = ResolvePath(request_path);
  if (!path.has_value()) {
    if (error != nullptr) {
      *error = "path not allowed";
    }
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(path->parent_path(), ec);

  std::ofstream file(*path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    if (error != nullptr) {
      *error = "unable to write file";
    }
    return false;
  }

  file.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!file.good()) {
    if (error != nullptr) {
      *error = "failed writing content";
    }
    return false;
  }
  return true;
}

}  // namespace ferryman::core
