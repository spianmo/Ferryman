#include "ferryman/docker/DockerManager.hpp"

#include "ferryman/util/Random.hpp"
#include "ferryman/util/StringUtil.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#endif

namespace ferryman::docker_runtime {

namespace {

using nlohmann::json;

constexpr int kMinLogTailLines = 1;
constexpr int kMaxLogTailLines = 2000;

struct CommandResult {
  int exit_code = -1;
  std::string output;
};

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

std::string QuoteShellArg(const std::string& value) {
  if (value.empty()) {
    return "''";
  }
  const bool is_safe = std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '/' || ch == ':' || ch == '=';
  });
  if (is_safe) {
    return value;
  }
  std::string quoted = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      quoted += "'\"'\"'";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

bool RunCommand(const std::vector<std::string>& args, CommandResult* result, std::string* error) {
  if (result == nullptr) {
    if (error != nullptr) {
      *error = "missing command result target";
    }
    return false;
  }
  if (args.empty()) {
    if (error != nullptr) {
      *error = "missing command";
    }
    return false;
  }

  std::ostringstream command;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0) {
      command << ' ';
    }
    command << QuoteShellArg(args[i]);
  }
  command << " 2>&1";

  FILE* pipe = OpenPipe(command.str());
  if (pipe == nullptr) {
    if (error != nullptr) {
      *error = "failed to launch command";
    }
    return false;
  }

  std::array<char, 4096> buffer{};
  std::string output;
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output.append(buffer.data());
  }

  const int status = ClosePipe(pipe);
  int exit_code = status;
#if defined(__unix__) || defined(__APPLE__)
  if (WIFEXITED(status)) {
    exit_code = WEXITSTATUS(status);
  }
#endif

  result->exit_code = exit_code;
  result->output = output;
  return exit_code == 0;
}

std::string ErrorFromCommandOutput(std::string output, const std::string& fallback) {
  output = util::Trim(output);
  if (!output.empty()) {
    return output;
  }
  return fallback;
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::vector<std::string> SplitLines(std::string_view text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string line(text.substr(start, end - start));
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      lines.push_back(std::move(line));
    }
    if (end == text.size()) {
      break;
    }
    start = end + 1;
  }
  return lines;
}

std::vector<std::string> SplitByTab(const std::string& line) {
  std::vector<std::string> fields;
  size_t start = 0;
  while (start <= line.size()) {
    const size_t pos = line.find('\t', start);
    if (pos == std::string::npos) {
      fields.push_back(line.substr(start));
      break;
    }
    fields.push_back(line.substr(start, pos - start));
    start = pos + 1;
  }
  return fields;
}

bool IsSafeContainerName(const std::string& name) {
  if (name.empty() || name.size() > 180) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.';
  });
}

void SkipSpaces(const std::string& text, size_t* cursor) {
  if (cursor == nullptr) {
    return;
  }
  while (*cursor < text.size() && std::isspace(static_cast<unsigned char>(text[*cursor])) != 0) {
    (*cursor)++;
  }
}

std::vector<std::string> SplitByWhitespace(const std::string& line) {
  std::vector<std::string> tokens;
  size_t cursor = 0;
  while (cursor < line.size()) {
    SkipSpaces(line, &cursor);
    if (cursor >= line.size()) {
      break;
    }
    const size_t start = cursor;
    while (cursor < line.size() && std::isspace(static_cast<unsigned char>(line[cursor])) == 0) {
      cursor++;
    }
    tokens.push_back(line.substr(start, cursor - start));
  }
  return tokens;
}

bool ParseProcessRow(const std::string& line, size_t column_count, std::vector<std::string>* row) {
  if (row == nullptr || column_count == 0) {
    return false;
  }
  row->clear();
  row->reserve(column_count);

  size_t cursor = 0;
  if (column_count == 1) {
    row->push_back(util::Trim(line));
    return !row->front().empty();
  }

  for (size_t idx = 0; idx + 1 < column_count; ++idx) {
    SkipSpaces(line, &cursor);
    if (cursor >= line.size()) {
      return false;
    }
    const size_t start = cursor;
    while (cursor < line.size() && std::isspace(static_cast<unsigned char>(line[cursor])) == 0) {
      cursor++;
    }
    row->push_back(line.substr(start, cursor - start));
  }

  SkipSpaces(line, &cursor);
  if (cursor >= line.size()) {
    row->push_back("");
  } else {
    row->push_back(util::Trim(line.substr(cursor)));
  }
  return true;
}

bool HasUnsafePathChars(const std::string& value) {
  return value.find('\0') != std::string::npos || value.find('\n') != std::string::npos ||
         value.find('\r') != std::string::npos;
}

std::string NormalizeContainerPath(std::string value) {
  value = util::Trim(value);
  if (value.empty()) {
    return "/";
  }
  std::replace(value.begin(), value.end(), '\\', '/');
  if (value.front() != '/') {
    value.insert(value.begin(), '/');
  }
  std::string collapsed;
  collapsed.reserve(value.size());
  bool last_slash = false;
  for (char ch : value) {
    if (ch == '/') {
      if (!last_slash || collapsed.empty()) {
        collapsed.push_back(ch);
      }
      last_slash = true;
    } else {
      collapsed.push_back(ch);
      last_slash = false;
    }
  }
  while (collapsed.size() > 1 && collapsed.back() == '/') {
    collapsed.pop_back();
  }
  if (collapsed.empty()) {
    return "/";
  }
  return collapsed;
}

bool ParseInt64(const std::string& text, int64_t* value) {
  if (value == nullptr) {
    return false;
  }
  try {
    *value = std::stoll(text);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseUint64(const std::string& text, uint64_t* value) {
  if (value == nullptr) {
    return false;
  }
  try {
    *value = static_cast<uint64_t>(std::stoull(text));
    return true;
  } catch (...) {
    return false;
  }
}

double ParsePercent(const std::string& text) {
  std::string trimmed = util::Trim(text);
  if (!trimmed.empty() && trimmed.back() == '%') {
    trimmed.pop_back();
  }
  try {
    return std::stod(trimmed);
  } catch (...) {
    return 0.0;
  }
}

uint64_t ParseByteSize(std::string value) {
  value = util::Trim(value);
  if (value.empty() || value == "0") {
    return 0;
  }
  value.erase(std::remove(value.begin(), value.end(), ','), value.end());
  size_t pos = 0;
  while (pos < value.size()) {
    const unsigned char c = static_cast<unsigned char>(value[pos]);
    if (std::isdigit(c) != 0 || c == '.') {
      ++pos;
      continue;
    }
    break;
  }
  if (pos == 0) {
    return 0;
  }

  double number = 0.0;
  try {
    number = std::stod(value.substr(0, pos));
  } catch (...) {
    return 0;
  }
  std::string unit = ToLower(util::Trim(value.substr(pos)));

  long double multiplier = 1.0L;
  if (unit == "b" || unit.empty()) {
    multiplier = 1.0L;
  } else if (unit == "k" || unit == "kb") {
    multiplier = 1'000.0L;
  } else if (unit == "m" || unit == "mb") {
    multiplier = 1'000'000.0L;
  } else if (unit == "g" || unit == "gb") {
    multiplier = 1'000'000'000.0L;
  } else if (unit == "t" || unit == "tb") {
    multiplier = 1'000'000'000'000.0L;
  } else if (unit == "p" || unit == "pb") {
    multiplier = 1'000'000'000'000'000.0L;
  } else if (unit == "ki" || unit == "kib") {
    multiplier = 1024.0L;
  } else if (unit == "mi" || unit == "mib") {
    multiplier = 1024.0L * 1024.0L;
  } else if (unit == "gi" || unit == "gib") {
    multiplier = 1024.0L * 1024.0L * 1024.0L;
  } else if (unit == "ti" || unit == "tib") {
    multiplier = 1024.0L * 1024.0L * 1024.0L * 1024.0L;
  } else if (unit == "pi" || unit == "pib") {
    multiplier = 1024.0L * 1024.0L * 1024.0L * 1024.0L * 1024.0L;
  } else {
    multiplier = 1.0L;
  }

  const long double bytes = std::max<long double>(0.0L, static_cast<long double>(number) * multiplier);
  return static_cast<uint64_t>(bytes + 0.5L);
}

void ParseBytePair(const std::string& text, uint64_t* first, uint64_t* second) {
  if (first != nullptr) {
    *first = 0;
  }
  if (second != nullptr) {
    *second = 0;
  }
  const size_t slash = text.find('/');
  if (slash == std::string::npos) {
    if (first != nullptr) {
      *first = ParseByteSize(text);
    }
    return;
  }
  if (first != nullptr) {
    *first = ParseByteSize(text.substr(0, slash));
  }
  if (second != nullptr) {
    *second = ParseByteSize(text.substr(slash + 1));
  }
}

bool ParseFileEntryLine(const std::string& line, const std::string& current_path, ContainerFileEntry* entry) {
  if (entry == nullptr) {
    return false;
  }
  const size_t tab1 = line.find('\t');
  if (tab1 == std::string::npos) {
    return false;
  }
  const size_t tab2 = line.find('\t', tab1 + 1);
  if (tab2 == std::string::npos) {
    return false;
  }
  const size_t tab3 = line.find('\t', tab2 + 1);
  if (tab3 == std::string::npos) {
    return false;
  }
  const size_t tab4 = line.find('\t', tab3 + 1);
  if (tab4 == std::string::npos) {
    return false;
  }

  const std::string type = line.substr(0, tab1);
  const std::string size_text = line.substr(tab1 + 1, tab2 - tab1 - 1);
  const std::string mtime_text = line.substr(tab2 + 1, tab3 - tab2 - 1);
  const std::string perms = line.substr(tab3 + 1, tab4 - tab3 - 1);
  const std::string name = line.substr(tab4 + 1);
  if (name.empty()) {
    return false;
  }

  entry->name = name;
  entry->is_directory = type == "d";
  entry->permissions = perms.empty() ? "---------" : perms;
  entry->size = 0;
  entry->modified_at = 0;
  if (!entry->is_directory) {
    ParseUint64(size_text, &entry->size);
  }
  ParseInt64(mtime_text, &entry->modified_at);
  entry->path = current_path == "/" ? "/" + name : current_path + "/" + name;
  return true;
}

std::string BaseNameFromPath(const std::string& normalized_path) {
  if (normalized_path.empty() || normalized_path == "/") {
    return "file.bin";
  }
  const size_t idx = normalized_path.find_last_of('/');
  if (idx == std::string::npos || idx + 1 >= normalized_path.size()) {
    return "file.bin";
  }
  std::string name = normalized_path.substr(idx + 1);
  if (name.empty()) {
    return "file.bin";
  }
  return name;
}

std::filesystem::path PrepareTempDir(const std::filesystem::path& workspace_root, std::string* error) {
  const std::filesystem::path base_dir = workspace_root / ".docker-tmp";
  std::error_code ec;
  std::filesystem::create_directories(base_dir, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "failed to create temp directory: " + ec.message();
    }
    return {};
  }
  const std::filesystem::path temp_dir = base_dir / ("io-" + util::RandomHex(8));
  std::filesystem::create_directories(temp_dir, ec);
  if (ec) {
    if (error != nullptr) {
      *error = "failed to create temp working directory: " + ec.message();
    }
    return {};
  }
  return temp_dir;
}

struct TempDirGuard {
  std::filesystem::path path;
  ~TempDirGuard() {
    if (path.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

}  // namespace

DockerManager::DockerManager(std::filesystem::path workspace_root) : workspace_root_(std::move(workspace_root)) {}

std::vector<ContainerInfo> DockerManager::ListContainers(bool include_all, std::string* error) const {
  std::vector<std::string> command = {
      "docker",
      "ps",
  };
  if (include_all) {
    command.push_back("-a");
  }
  command.push_back("--format");
  command.push_back("{{.ID}}\t{{.Names}}\t{{.Image}}\t{{.State}}\t{{.Status}}\t{{.RunningFor}}\t{{.Ports}}\t{{.CreatedAt}}");

  CommandResult result;
  if (!RunCommand(command, &result, error)) {
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(result.output, "failed to list docker containers");
    }
    return {};
  }

  std::vector<ContainerInfo> containers;
  for (const auto& line : SplitLines(result.output)) {
    const auto fields = SplitByTab(line);
    if (fields.size() < 8) {
      continue;
    }
    ContainerInfo container;
    container.id = fields[0];
    container.name = fields[1];
    container.image = fields[2];
    container.state = fields[3];
    container.status = fields[4];
    container.running_for = fields[5];
    container.ports = fields[6];
    container.created_at = fields[7];
    if (!container.name.empty()) {
      containers.push_back(std::move(container));
    }
  }

  std::stable_sort(containers.begin(), containers.end(), [](const ContainerInfo& a, const ContainerInfo& b) {
    const bool a_running = ToLower(a.state) == "running";
    const bool b_running = ToLower(b.state) == "running";
    if (a_running != b_running) {
      return a_running;
    }
    return a.name < b.name;
  });
  return containers;
}

bool DockerManager::StartContainer(const std::string& name, std::string* error) const {
  const std::string trimmed = util::Trim(name);
  if (!IsSafeContainerName(trimmed)) {
    if (error != nullptr) {
      *error = "invalid container name";
    }
    return false;
  }
  CommandResult result;
  if (!RunCommand({"docker", "start", trimmed}, &result, error)) {
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(result.output, "failed to start container");
    }
    return false;
  }
  return true;
}

bool DockerManager::StopContainer(const std::string& name, std::string* error) const {
  const std::string trimmed = util::Trim(name);
  if (!IsSafeContainerName(trimmed)) {
    if (error != nullptr) {
      *error = "invalid container name";
    }
    return false;
  }
  CommandResult result;
  if (!RunCommand({"docker", "stop", trimmed}, &result, error)) {
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(result.output, "failed to stop container");
    }
    return false;
  }
  return true;
}

bool DockerManager::RestartContainer(const std::string& name, std::string* error) const {
  const std::string trimmed = util::Trim(name);
  if (!IsSafeContainerName(trimmed)) {
    if (error != nullptr) {
      *error = "invalid container name";
    }
    return false;
  }
  CommandResult result;
  if (!RunCommand({"docker", "restart", trimmed}, &result, error)) {
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(result.output, "failed to restart container");
    }
    return false;
  }
  return true;
}

bool DockerManager::GetLogs(const std::string& name, int tail_lines, std::string* logs, std::string* error) const {
  const std::string trimmed = util::Trim(name);
  if (!IsSafeContainerName(trimmed)) {
    if (error != nullptr) {
      *error = "invalid container name";
    }
    return false;
  }
  const int safe_tail = std::clamp(tail_lines, kMinLogTailLines, kMaxLogTailLines);
  CommandResult result;
  if (!RunCommand({"docker", "logs", "--tail", std::to_string(safe_tail), trimmed}, &result, error)) {
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(result.output, "failed to fetch container logs");
    }
    return false;
  }
  if (logs != nullptr) {
    *logs = result.output;
  }
  return true;
}

bool DockerManager::InspectContainer(const std::string& name, std::string* inspect, std::string* error) const {
  const std::string trimmed = util::Trim(name);
  if (!IsSafeContainerName(trimmed)) {
    if (error != nullptr) {
      *error = "invalid container name";
    }
    return false;
  }
  CommandResult result;
  if (!RunCommand({"docker", "inspect", trimmed}, &result, error)) {
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(result.output, "failed to inspect container");
    }
    return false;
  }
  if (inspect != nullptr) {
    *inspect = result.output;
  }
  return true;
}

bool DockerManager::GetStats(const std::string& name, ContainerStats* stats, std::string* error) const {
  const std::string trimmed = util::Trim(name);
  if (!IsSafeContainerName(trimmed)) {
    if (error != nullptr) {
      *error = "invalid container name";
    }
    return false;
  }
  if (stats == nullptr) {
    if (error != nullptr) {
      *error = "missing stats output target";
    }
    return false;
  }

  CommandResult result;
  if (!RunCommand({"docker", "stats", "--no-stream", "--format", "{{json .}}", trimmed}, &result, error)) {
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(result.output, "failed to fetch container stats");
    }
    return false;
  }

  std::string json_line;
  for (const auto& line : SplitLines(result.output)) {
    if (!line.empty()) {
      json_line = line;
      break;
    }
  }
  if (json_line.empty()) {
    if (error != nullptr) {
      *error = "empty docker stats payload";
    }
    return false;
  }

  const json payload = json::parse(json_line, nullptr, false);
  if (payload.is_discarded() || !payload.is_object()) {
    if (error != nullptr) {
      *error = "failed to parse docker stats payload";
    }
    return false;
  }

  stats->name = payload.value("Name", trimmed);
  stats->cpu_percent = ParsePercent(payload.value("CPUPerc", std::string{}));
  stats->memory_percent = ParsePercent(payload.value("MemPerc", std::string{}));
  ParseBytePair(payload.value("MemUsage", std::string{}), &stats->memory_usage_bytes, &stats->memory_limit_bytes);
  ParseBytePair(payload.value("NetIO", std::string{}), &stats->net_input_bytes, &stats->net_output_bytes);
  ParseBytePair(payload.value("BlockIO", std::string{}), &stats->block_input_bytes, &stats->block_output_bytes);
  stats->pids = 0;
  const std::string pids_text = util::Trim(payload.value("PIDs", std::string{}));
  if (!pids_text.empty()) {
    try {
      stats->pids = std::stoi(pids_text);
    } catch (...) {
      stats->pids = 0;
    }
  }

  return true;
}

bool DockerManager::GetProcesses(const std::string& name, int max_rows,
                                 ContainerProcessSnapshot* processes, std::string* error) const {
  const std::string trimmed = util::Trim(name);
  if (!IsSafeContainerName(trimmed)) {
    if (error != nullptr) {
      *error = "invalid container name";
    }
    return false;
  }
  if (processes == nullptr) {
    if (error != nullptr) {
      *error = "missing process output target";
    }
    return false;
  }

  CommandResult result;
  const std::vector<std::string> rich_command = {
      "docker",
      "top",
      trimmed,
      "-eo",
      "pid,ppid,user,pcpu,pmem,etime,comm,args",
  };
  bool ok = RunCommand(rich_command, &result, error);
  if (!ok) {
    // Fallback to docker top default columns when extended ps format is not available.
    ok = RunCommand({"docker", "top", trimmed}, &result, error);
  }
  if (!ok) {
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(result.output, "failed to fetch container processes");
    }
    return false;
  }

  const auto lines = SplitLines(result.output);
  if (lines.empty()) {
    if (error != nullptr) {
      *error = "empty docker top output";
    }
    return false;
  }

  const std::vector<std::string> columns = SplitByWhitespace(lines.front());
  if (columns.empty()) {
    if (error != nullptr) {
      *error = "invalid docker top header";
    }
    return false;
  }

  const size_t safe_max_rows = static_cast<size_t>(std::clamp(max_rows, 1, 500));
  std::vector<std::vector<std::string>> rows;
  rows.reserve(std::min(safe_max_rows, lines.size() > 0 ? lines.size() - 1 : 0));

  for (size_t idx = 1; idx < lines.size(); ++idx) {
    if (rows.size() >= safe_max_rows) {
      break;
    }
    std::vector<std::string> row;
    if (!ParseProcessRow(lines[idx], columns.size(), &row)) {
      continue;
    }
    rows.push_back(std::move(row));
  }

  processes->columns = columns;
  processes->rows = std::move(rows);
  return true;
}

bool DockerManager::ListFiles(const std::string& name, const std::string& path,
                              std::vector<ContainerFileEntry>* entries, std::string* current_path,
                              std::string* error) const {
  const std::string trimmed_name = util::Trim(name);
  if (!IsSafeContainerName(trimmed_name)) {
    if (error != nullptr) {
      *error = "invalid container name";
    }
    return false;
  }
  if (HasUnsafePathChars(path)) {
    if (error != nullptr) {
      *error = "invalid path";
    }
    return false;
  }
  const std::string normalized_path = NormalizeContainerPath(path);

  const std::string script = R"(target="$1"; if [ -z "$target" ]; then target="/"; fi; if [ ! -d "$target" ]; then echo "__ERR__NOTDIR"; exit 11; fi; printf "__PATH__\t%s\n" "$target"; for item in "$target"/* "$target"/.[!.]* "$target"/..?*; do [ -e "$item" ] || continue; name="${item##*/}"; if [ -d "$item" ]; then type="d"; size=0; else type="f"; size=$(wc -c < "$item" 2>/dev/null || echo 0); fi; mtime=$(date -r "$item" +%s 2>/dev/null || stat -c %Y "$item" 2>/dev/null || stat -f %m "$item" 2>/dev/null || echo 0); perms=$(ls -ld "$item" 2>/dev/null | awk '{print $1}'); if [ -z "$perms" ]; then perms="---------"; fi; printf "%s\t%s\t%s\t%s\t%s\n" "$type" "$size" "$mtime" "$perms" "$name"; done)";

  CommandResult result;
  if (!RunCommand({"docker", "exec", trimmed_name, "sh", "-lc", script, "--", normalized_path}, &result, error)) {
    const std::string output = util::Trim(result.output);
    if (output.find("__ERR__NOTDIR") != std::string::npos) {
      if (error != nullptr) {
        *error = "path is not a directory";
      }
      return false;
    }
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(output, "failed to list container files");
    }
    return false;
  }

  std::string resolved_path = normalized_path;
  std::vector<ContainerFileEntry> parsed;
  for (const auto& line : SplitLines(result.output)) {
    if (line.rfind("__PATH__\t", 0) == 0) {
      resolved_path = NormalizeContainerPath(line.substr(8));
      continue;
    }
    ContainerFileEntry entry;
    if (!ParseFileEntryLine(line, resolved_path, &entry)) {
      continue;
    }
    parsed.push_back(std::move(entry));
  }

  std::stable_sort(parsed.begin(), parsed.end(), [](const ContainerFileEntry& a, const ContainerFileEntry& b) {
    if (a.is_directory != b.is_directory) {
      return a.is_directory;
    }
    return a.name < b.name;
  });

  if (entries != nullptr) {
    *entries = std::move(parsed);
  }
  if (current_path != nullptr) {
    *current_path = resolved_path;
  }
  return true;
}

bool DockerManager::ReadFile(const std::string& name, const std::string& path, std::string* content,
                             std::string* error) const {
  const std::string trimmed_name = util::Trim(name);
  if (!IsSafeContainerName(trimmed_name)) {
    if (error != nullptr) {
      *error = "invalid container name";
    }
    return false;
  }
  if (HasUnsafePathChars(path)) {
    if (error != nullptr) {
      *error = "invalid path";
    }
    return false;
  }
  const std::string normalized_path = NormalizeContainerPath(path);
  if (normalized_path == "/") {
    if (error != nullptr) {
      *error = "path must target a file";
    }
    return false;
  }

  std::string temp_error;
  const std::filesystem::path temp_dir = PrepareTempDir(workspace_root_, &temp_error);
  if (temp_dir.empty()) {
    if (error != nullptr) {
      *error = temp_error.empty() ? "failed to prepare temp directory" : temp_error;
    }
    return false;
  }
  TempDirGuard cleanup{temp_dir};

  CommandResult cp_result;
  if (!RunCommand({"docker", "cp", trimmed_name + ":" + normalized_path, temp_dir.string()}, &cp_result, error)) {
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(cp_result.output, "failed to copy file from container");
    }
    return false;
  }

  std::filesystem::path copied_file;
  for (std::filesystem::recursive_directory_iterator it(temp_dir), end; it != end; ++it) {
    std::error_code ec;
    if (!it->is_regular_file(ec) || ec) {
      continue;
    }
    copied_file = it->path();
    break;
  }
  if (copied_file.empty()) {
    if (error != nullptr) {
      *error = "selected path is not a readable file";
    }
    return false;
  }

  std::ifstream input(copied_file, std::ios::binary);
  if (!input.is_open()) {
    if (error != nullptr) {
      *error = "failed to open copied file";
    }
    return false;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  if (content != nullptr) {
    *content = buffer.str();
  }
  return true;
}

bool DockerManager::WriteFile(const std::string& name, const std::string& path, const std::string& content,
                              std::string* error) const {
  const std::string trimmed_name = util::Trim(name);
  if (!IsSafeContainerName(trimmed_name)) {
    if (error != nullptr) {
      *error = "invalid container name";
    }
    return false;
  }
  if (HasUnsafePathChars(path)) {
    if (error != nullptr) {
      *error = "invalid path";
    }
    return false;
  }
  const std::string normalized_path = NormalizeContainerPath(path);
  if (normalized_path == "/") {
    if (error != nullptr) {
      *error = "path must target a file";
    }
    return false;
  }

  std::string temp_error;
  const std::filesystem::path temp_dir = PrepareTempDir(workspace_root_, &temp_error);
  if (temp_dir.empty()) {
    if (error != nullptr) {
      *error = temp_error.empty() ? "failed to prepare temp directory" : temp_error;
    }
    return false;
  }
  TempDirGuard cleanup{temp_dir};

  const std::string file_name = BaseNameFromPath(normalized_path);
  const std::filesystem::path local_file = temp_dir / file_name;
  std::ofstream output(local_file, std::ios::binary);
  if (!output.is_open()) {
    if (error != nullptr) {
      *error = "failed to open temp file for writing";
    }
    return false;
  }
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  output.close();

  CommandResult cp_result;
  if (!RunCommand({"docker", "cp", local_file.string(), trimmed_name + ":" + normalized_path}, &cp_result, error)) {
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(cp_result.output, "failed to write file into container");
    }
    return false;
  }
  return true;
}

}  // namespace ferryman::docker_runtime
