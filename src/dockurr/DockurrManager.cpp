#include "ferryman/dockurr/DockurrManager.hpp"

#include "ferryman/util/Random.hpp"
#include "ferryman/util/StringUtil.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#endif

namespace ferryman::dockurr {

namespace {

using nlohmann::json;

constexpr const char* kDockurrWindowsImage = "dockurr/windows";
constexpr const char* kDockurrMacosImage = "dockurr/macos";
constexpr const char* kDockurWindowsImage = "dockur/windows";
constexpr const char* kDockurMacosImage = "dockur/macos";
constexpr int kMinLogTailLines = 1;
constexpr int kMaxLogTailLines = 500;

struct CommandResult {
  int exit_code = -1;
  std::string output;
};

struct PsRow {
  std::string id;
  std::string image;
  std::string name;
  std::string ports;
  std::string running_for;
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
  const bool is_safe =
      std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '/' || ch == ':' || ch == '=';
      });
  if (is_safe) {
    return value;
  }
  std::string quoted = "'";
  for (char ch : value) {
    if (ch == '\'') {
      quoted += "'\"'\"'";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

bool RunCommand(const std::vector<std::string>& args, CommandResult* result, std::string* error,
                const DockurrManager::LogCallback& callback = nullptr) {
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

  std::array<char, 2048> buffer{};
  std::string output;
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output.append(buffer.data());
    if (callback) {
      callback(buffer.data());
    }
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

void EmitLogChunkLines(const std::string& chunk, const DockurrManager::LogCallback& callback) {
  if (!callback || chunk.empty()) {
    return;
  }
  const auto lines = SplitLines(chunk);
  for (const auto& line : lines) {
    callback(line);
  }
}

void EmitLogDiff(const std::string& previous, const std::string& current, const DockurrManager::LogCallback& callback) {
  if (!callback || current.empty()) {
    return;
  }
  if (current.size() >= previous.size() && current.compare(0, previous.size(), previous) == 0) {
    EmitLogChunkLines(current.substr(previous.size()), callback);
    return;
  }
  EmitLogChunkLines(current, callback);
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

bool ParsePsRow(const std::string& line, PsRow* row) {
  if (row == nullptr) {
    return false;
  }
  const auto fields = SplitByTab(line);
  if (fields.size() < 5) {
    return false;
  }
  row->id = fields[0];
  row->image = fields[1];
  row->name = fields[2];
  row->ports = fields[3];
  row->running_for = fields[4];
  return !row->id.empty();
}

bool IsValidVmName(const std::string& value) {
  if (value.empty() || value.size() > 80) {
    return false;
  }
  for (char ch : value) {
    const unsigned char c = static_cast<unsigned char>(ch);
    const bool ok = std::isalnum(c) != 0 || ch == '-' || ch == '_' || ch == '.';
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::string SanitizeNameSegment(std::string value) {
  for (char& ch : value) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) != 0) {
      ch = static_cast<char>(std::tolower(c));
    } else {
      ch = '-';
    }
  }
  while (!value.empty() && value.front() == '-') {
    value.erase(value.begin());
  }
  while (!value.empty() && value.back() == '-') {
    value.pop_back();
  }
  if (value.empty()) {
    return "vm";
  }
  return value;
}

std::string BuildDefaultVmName(const std::string& os, const std::string& version) {
  std::string suffix = util::RandomHex(3);
  std::string normalized_version = SanitizeNameSegment(version);
  if (normalized_version.size() > 12) {
    normalized_version = normalized_version.substr(0, 12);
  }
  return "dockurr-" + os + normalized_version + "-" + suffix;
}

bool ResolveImageAndDesktopPort(const std::string& os, std::string* image, std::string* desktop_port,
                                std::string* error) {
  if (os == "windows") {
    if (image != nullptr) {
      *image = kDockurrWindowsImage;
    }
    if (desktop_port != nullptr) {
      *desktop_port = "3389";
    }
    return true;
  }
  if (os == "macos") {
    if (image != nullptr) {
      *image = kDockurrMacosImage;
    }
    if (desktop_port != nullptr) {
      *desktop_port = "5900";
    }
    return true;
  }
  if (error != nullptr) {
    *error = "invalid os, expected windows or macos";
  }
  return false;
}

std::string RemoveNamePrefix(std::string name) {
  if (!name.empty() && name.front() == '/') {
    name.erase(name.begin());
  }
  return name;
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool IsTruthy(std::string value) {
  value = ToLower(util::Trim(std::move(value)));
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool IsDockurrImage(std::string image) {
  image = ToLower(std::move(image));
  return image.find(kDockurrWindowsImage) != std::string::npos ||
         image.find(kDockurrMacosImage) != std::string::npos ||
         image.find(kDockurWindowsImage) != std::string::npos ||
         image.find(kDockurMacosImage) != std::string::npos;
}

bool IsManagedDockurrLabel(const json& inspect_item) {
  if (!inspect_item.is_object() || !inspect_item.contains("Config") || !inspect_item["Config"].is_object()) {
    return false;
  }
  const auto& config = inspect_item["Config"];
  if (!config.contains("Labels") || !config["Labels"].is_object()) {
    return false;
  }
  const auto& labels = config["Labels"];
  if (!labels.contains("ferryman.dockurr.managed")) {
    return false;
  }
  if (labels["ferryman.dockurr.managed"].is_boolean()) {
    return labels["ferryman.dockurr.managed"].get<bool>();
  }
  if (labels["ferryman.dockurr.managed"].is_string()) {
    return IsTruthy(labels["ferryman.dockurr.managed"].get<std::string>());
  }
  return false;
}

std::string DetectOsFromImage(const std::string& image) {
  const std::string lower = ToLower(image);
  if (lower.find(kDockurrWindowsImage) != std::string::npos || lower.find(kDockurWindowsImage) != std::string::npos) {
    return "windows";
  }
  if (lower.find(kDockurrMacosImage) != std::string::npos || lower.find(kDockurMacosImage) != std::string::npos) {
    return "macos";
  }
  return "";
}

std::string HostPortFromInspect(const json& inspect_item, const std::string& container_port) {
  if (!inspect_item.is_object()) {
    return "";
  }
  if (!inspect_item.contains("NetworkSettings")) {
    return "";
  }
  const auto& network = inspect_item["NetworkSettings"];
  if (!network.is_object() || !network.contains("Ports")) {
    return "";
  }
  const auto& ports = network["Ports"];
  if (!ports.is_object() || !ports.contains(container_port)) {
    return "";
  }
  const auto& published = ports[container_port];
  if (!published.is_array() || published.empty()) {
    return "";
  }
  const auto& first = published.front();
  if (!first.is_object() || !first.contains("HostPort") || !first["HostPort"].is_string()) {
    return "";
  }
  return first["HostPort"].get<std::string>();
}

bool IsPersistentVm(const json& inspect_item) {
  if (inspect_item.contains("Config") && inspect_item["Config"].is_object()) {
    const auto& config = inspect_item["Config"];
    if (config.contains("Labels") && config["Labels"].is_object()) {
      const auto& labels = config["Labels"];
      if (labels.contains("ferryman.dockurr.persist") && labels["ferryman.dockurr.persist"].is_string()) {
        const std::string persist = labels["ferryman.dockurr.persist"].get<std::string>();
        if (persist == "true" || persist == "1") {
          return true;
        }
      }
    }
  }

  if (!inspect_item.contains("Mounts") || !inspect_item["Mounts"].is_array()) {
    return false;
  }
  const auto& mounts = inspect_item["Mounts"];
  for (const auto& mount : mounts) {
    if (!mount.is_object()) {
      continue;
    }
    if (mount.contains("Source") && mount["Source"].is_string()) {
      const std::string source = mount["Source"].get<std::string>();
      if (source.find("vmdata-") != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

json FindInspectItemByIdPrefix(const json& inspect_payload, const std::string& id_prefix) {
  if (!inspect_payload.is_array()) {
    return nullptr;
  }
  for (const auto& item : inspect_payload) {
    if (!item.is_object() || !item.contains("Id") || !item["Id"].is_string()) {
      continue;
    }
    const std::string full_id = item["Id"].get<std::string>();
    if (full_id.rfind(id_prefix, 0) == 0) {
      return item;
    }
  }
  return nullptr;
}

}  // namespace

DockurrManager::DockurrManager(std::filesystem::path workspace_root) : workspace_root_(std::move(workspace_root)) {}

std::vector<VmInfo> DockurrManager::ListVms(std::string* error) const {
  CommandResult ps_result;
  const bool ok = RunCommand(
      {
          "docker",
          "ps",
          "-a",
          "--format",
          "{{.ID}}\t{{.Image}}\t{{.Names}}\t{{.Ports}}\t{{.RunningFor}}",
      },
      &ps_result, error);
  if (!ok) {
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(ps_result.output, "failed to list dockurr containers");
    }
    return {};
  }

  const auto lines = SplitLines(ps_result.output);
  std::vector<PsRow> rows;
  rows.reserve(lines.size());
  for (const auto& line : lines) {
    PsRow row;
    if (ParsePsRow(line, &row)) {
      rows.push_back(std::move(row));
    }
  }
  if (rows.empty()) {
    return {};
  }

  std::vector<std::string> inspect_command{
      "docker",
      "inspect",
  };
  inspect_command.reserve(rows.size() + 2);
  for (const auto& row : rows) {
    inspect_command.push_back(row.id);
  }

  CommandResult inspect_result;
  if (!RunCommand(inspect_command, &inspect_result, error)) {
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(inspect_result.output, "failed to inspect dockurr containers");
    }
    return {};
  }

  const json inspect_payload = json::parse(inspect_result.output, nullptr, false);
  if (inspect_payload.is_discarded() || !inspect_payload.is_array()) {
    if (error != nullptr) {
      *error = "failed to parse docker inspect output";
    }
    return {};
  }

  std::vector<VmInfo> vms;
  vms.reserve(rows.size());
  for (const auto& row : rows) {
    const json inspect_item = FindInspectItemByIdPrefix(inspect_payload, row.id);
    VmInfo vm;
    vm.id = row.id;
    vm.name = row.name;
    vm.os = DetectOsFromImage(row.image);
    vm.image = row.image;
    vm.ports = row.ports;
    vm.running_for = row.running_for;
    bool include = IsDockurrImage(vm.image);
    if (!inspect_item.is_null() && inspect_item.is_object() && IsManagedDockurrLabel(inspect_item)) {
      include = true;
    }
    if (!inspect_item.is_null() && inspect_item.is_object()) {
      if (inspect_item.contains("Name") && inspect_item["Name"].is_string()) {
        vm.name = RemoveNamePrefix(inspect_item["Name"].get<std::string>());
      }
      if (inspect_item.contains("Config") && inspect_item["Config"].is_object()) {
        const auto& config = inspect_item["Config"];
        if (config.contains("Image") && config["Image"].is_string()) {
          vm.image = config["Image"].get<std::string>();
          vm.os = DetectOsFromImage(vm.image);
          include = include || IsDockurrImage(vm.image);
        }
      }
      vm.novnc_port = HostPortFromInspect(inspect_item, "8006/tcp");
      if (vm.os == "windows") {
        vm.desktop_port = HostPortFromInspect(inspect_item, "3389/tcp");
      } else if (vm.os == "macos") {
        vm.desktop_port = HostPortFromInspect(inspect_item, "5900/tcp");
      }
      vm.persistent = IsPersistentVm(inspect_item);
    }
    if (!include) {
      continue;
    }
    vms.push_back(std::move(vm));
  }
  return vms;
}

bool DockurrManager::CreateVm(const CreateVmRequest& request, VmInfo* created_vm, std::string* error) const {
  std::string image;
  std::string desktop_port;
  if (!ResolveImageAndDesktopPort(request.os, &image, &desktop_port, error)) {
    return false;
  }
  const std::string version = util::Trim(request.version);
  if (version.empty()) {
    if (error != nullptr) {
      *error = "version is required";
    }
    return false;
  }

  std::string ram_size = util::Trim(request.ram_size);
  if (ram_size.empty()) {
    ram_size = "4G";
  }
  std::string disk_size = util::Trim(request.disk_size);
  if (disk_size.empty()) {
    disk_size = "64G";
  }

  std::string name = util::Trim(request.name);
  if (name.empty()) {
    name = BuildDefaultVmName(request.os, version);
  }
  if (!IsValidVmName(name)) {
    if (error != nullptr) {
      *error = "invalid vm name";
    }
    return false;
  }

  std::vector<std::string> run_command{
      "docker",          "run",      "-d",      "--name",     name,
      "--device",        "/dev/kvm", "--device", "/dev/net/tun", "--cap-add", "NET_ADMIN",
      "--label",         "ferryman.dockurr.managed=true",
      "--label",         request.persistent ? "ferryman.dockurr.persist=true" : "ferryman.dockurr.persist=false",
      "-p",              "0:8006",
      "-p",              "0:" + desktop_port,
  };

  if (request.persistent) {
    const std::filesystem::path vmdata_dir = workspace_root_ / ("vmdata-" + name);
    std::error_code ec;
    std::filesystem::create_directories(vmdata_dir, ec);
    if (ec) {
      if (error != nullptr) {
        *error = "failed to prepare persistent storage: " + ec.message();
      }
      return false;
    }
    run_command.push_back("-v");
    run_command.push_back(vmdata_dir.string() + ":/storage");
  }

  run_command.push_back("-e");
  run_command.push_back("VERSION=" + version);
  run_command.push_back("-e");
  run_command.push_back("RAM_SIZE=" + ram_size);
  run_command.push_back("-e");
  run_command.push_back("DISK_SIZE=" + disk_size);
  run_command.push_back(image);

  CommandResult run_result;
  if (!RunCommand(run_command, &run_result, error)) {
    if (error != nullptr) {
      *error = "failed to create vm: " + ErrorFromCommandOutput(run_result.output, "docker run failed");
    }
    return false;
  }

  if (created_vm == nullptr) {
    return true;
  }
  if (LookupVmByName(name, created_vm, error)) {
    return true;
  }

  created_vm->name = name;
  created_vm->os = request.os;
  created_vm->image = image;
  created_vm->persistent = request.persistent;
  return true;
}

bool DockurrManager::CreateVmWithStartupLogs(const CreateVmRequest& request, int max_wait_seconds,
                                             const LogCallback& log_callback, VmInfo* created_vm,
                                             std::string* error) const {
  VmInfo vm;
  if (log_callback) {
    log_callback("creating vm...");
  }
  if (!CreateVm(request, &vm, error)) {
    if (log_callback && error != nullptr) {
      log_callback("create failed: " + *error);
    }
    return false;
  }
  if (log_callback) {
    log_callback("vm created: " + vm.name);
  }

  if (!log_callback) {
    if (created_vm != nullptr) {
      *created_vm = vm;
    }
    return true;
  }

  const int safe_wait_seconds = std::clamp(max_wait_seconds, 5, 180);
  std::string previous_logs;
  bool ready = !vm.novnc_port.empty();

  for (int elapsed = 0; elapsed < safe_wait_seconds; ++elapsed) {
    std::string logs;
    std::string logs_error;
    if (GetLogs(vm.name, kMaxLogTailLines, &logs, &logs_error)) {
      EmitLogDiff(previous_logs, logs, log_callback);
      previous_logs = logs;
    } else if (elapsed == 0 && !logs_error.empty()) {
      log_callback("startup logs unavailable: " + logs_error);
    }

    VmInfo latest_vm;
    std::string lookup_error;
    if (LookupVmByName(vm.name, &latest_vm, &lookup_error)) {
      vm = latest_vm;
      if (!vm.novnc_port.empty()) {
        ready = true;
        break;
      }
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  if (ready) {
    log_callback("startup ready: noVNC " + vm.novnc_port);
  } else {
    log_callback("startup still in progress, noVNC port pending");
  }

  if (created_vm != nullptr) {
    *created_vm = vm;
  }
  return true;
}

bool DockurrManager::StopVm(const std::string& name, std::string* error) const {
  const std::string trimmed = util::Trim(name);
  if (!IsValidVmName(trimmed)) {
    if (error != nullptr) {
      *error = "invalid vm name";
    }
    return false;
  }
  CommandResult result;
  if (!RunCommand({"docker", "stop", trimmed}, &result, error)) {
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(result.output, "failed to stop vm");
    }
    return false;
  }
  return true;
}

bool DockurrManager::RestartVm(const std::string& name, std::string* error) const {
  const std::string trimmed = util::Trim(name);
  if (!IsValidVmName(trimmed)) {
    if (error != nullptr) {
      *error = "invalid vm name";
    }
    return false;
  }
  CommandResult result;
  if (!RunCommand({"docker", "restart", trimmed}, &result, error)) {
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(result.output, "failed to restart vm");
    }
    return false;
  }
  return true;
}

bool DockurrManager::GetLogs(const std::string& name, int tail_lines, std::string* logs, std::string* error) const {
  const std::string trimmed = util::Trim(name);
  if (!IsValidVmName(trimmed)) {
    if (error != nullptr) {
      *error = "invalid vm name";
    }
    return false;
  }
  const int safe_tail = std::clamp(tail_lines, kMinLogTailLines, kMaxLogTailLines);
  CommandResult result;
  if (!RunCommand({"docker", "logs", "--tail", std::to_string(safe_tail), trimmed}, &result, error)) {
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(result.output, "failed to fetch vm logs");
    }
    return false;
  }
  if (logs != nullptr) {
    *logs = result.output;
  }
  return true;
}

bool DockurrManager::InspectVm(const std::string& name, std::string* inspect, std::string* error) const {
  const std::string trimmed = util::Trim(name);
  if (!IsValidVmName(trimmed)) {
    if (error != nullptr) {
      *error = "invalid vm name";
    }
    return false;
  }
  CommandResult result;
  if (!RunCommand({"docker", "inspect", trimmed}, &result, error)) {
    if (error != nullptr) {
      *error = ErrorFromCommandOutput(result.output, "failed to inspect vm");
    }
    return false;
  }
  if (inspect != nullptr) {
    *inspect = result.output;
  }
  return true;
}

bool DockurrManager::StopTemporaryVms(std::vector<std::string>* stopped_names, std::string* error) const {
  std::string list_error;
  const auto vms = ListVms(&list_error);
  if (!list_error.empty()) {
    if (error != nullptr) {
      *error = list_error;
    }
    return false;
  }

  std::vector<std::string> failures;
  for (const auto& vm : vms) {
    if (vm.persistent) {
      continue;
    }
    std::string stop_error;
    if (!StopVm(vm.name, &stop_error)) {
      failures.push_back(vm.name + ": " + stop_error);
      continue;
    }
    if (stopped_names != nullptr) {
      stopped_names->push_back(vm.name);
    }
  }

  if (!failures.empty()) {
    if (error != nullptr) {
      std::ostringstream message;
      message << "failed to stop temporary vm(s): ";
      for (size_t i = 0; i < failures.size(); ++i) {
        if (i > 0) {
          message << "; ";
        }
        message << failures[i];
      }
      *error = message.str();
    }
    return false;
  }
  return true;
}

bool DockurrManager::LookupVmByName(const std::string& name, VmInfo* vm, std::string* error) const {
  if (vm == nullptr) {
    if (error != nullptr) {
      *error = "missing vm output target";
    }
    return false;
  }
  std::string list_error;
  const auto vms = ListVms(&list_error);
  if (!list_error.empty()) {
    if (error != nullptr) {
      *error = list_error;
    }
    return false;
  }
  for (const auto& candidate : vms) {
    if (candidate.name == name) {
      *vm = candidate;
      return true;
    }
  }
  if (error != nullptr) {
    *error = "vm not found after creation";
  }
  return false;
}

}  // namespace ferryman::dockurr
