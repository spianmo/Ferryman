#include "ferryman/web/SystemMonitor.hpp"

#include "ferryman/util/StringUtil.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <sstream>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#include <mach/vm_statistics.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>
#else
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace ferryman::web {

namespace {

using nlohmann::json;

std::string TrimCopy(const std::string& value) {
  return util::Trim(value);
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string RunCommandCapture(const char* command) {
  if (command == nullptr || command[0] == '\0') {
    return "";
  }
#if defined(_WIN32)
  FILE* pipe = ::_popen(command, "r");
#else
  FILE* pipe = ::popen(command, "r");
#endif
  if (pipe == nullptr) {
    return "";
  }

  std::string output;
  char buffer[512];
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output.append(buffer);
  }

#if defined(_WIN32)
  (void)::_pclose(pipe);
#else
  (void)::pclose(pipe);
#endif
  return output;
}

std::string FirstNonEmptyLine(const std::string& text) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const std::string trimmed = TrimCopy(line);
    if (!trimmed.empty()) {
      return trimmed;
    }
  }
  return "";
}

std::optional<double> ParseDouble(const std::string& raw) {
  try {
    size_t consumed = 0;
    const double value = std::stod(raw, &consumed);
    if (consumed == 0) {
      return std::nullopt;
    }
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

int ParseFirstInt(const std::string& text) {
  for (size_t idx = 0; idx < text.size(); ++idx) {
    if (!std::isdigit(static_cast<unsigned char>(text[idx]))) {
      continue;
    }
    size_t end = idx;
    while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) {
      ++end;
    }
    try {
      return std::stoi(text.substr(idx, end - idx));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

std::string ReadFirstLine(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return "";
  }
  std::string line;
  std::getline(file, line);
  line.erase(std::remove(line.begin(), line.end(), '\0'), line.end());
  return TrimCopy(line);
}

std::string JoinParts(const std::vector<std::string>& parts, const std::string& delimiter) {
  std::ostringstream out;
  bool first = true;
  for (const auto& part : parts) {
    if (part.empty()) {
      continue;
    }
    if (!first) {
      out << delimiter;
    }
    first = false;
    out << part;
  }
  return out.str();
}

std::string FormatBytesCompact(uint64_t bytes) {
  static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  double value = static_cast<double>(bytes);
  size_t unit_index = 0;
  while (value >= 1024.0 && unit_index + 1 < sizeof(kUnits) / sizeof(kUnits[0])) {
    value /= 1024.0;
    ++unit_index;
  }

  std::ostringstream out;
  if (value >= 100.0 || unit_index == 0) {
    out << std::fixed << std::setprecision(0);
  } else {
    out << std::fixed << std::setprecision(1);
  }
  out << value << ' ' << kUnits[unit_index];
  return out.str();
}

double LinuxCpuFrequencyMhz() {
#if defined(__linux__)
  std::ifstream file("/proc/cpuinfo");
  if (!file.is_open()) {
    return 0.0;
  }
  std::string line;
  double sum_mhz = 0.0;
  int count = 0;
  while (std::getline(file, line)) {
    const std::string trimmed = TrimCopy(line);
    if (!StartsWith(trimmed, "cpu MHz")) {
      continue;
    }
    const size_t colon = trimmed.find(':');
    if (colon == std::string::npos || colon + 1 >= trimmed.size()) {
      continue;
    }
    const auto parsed = ParseDouble(TrimCopy(trimmed.substr(colon + 1)));
    if (!parsed.has_value()) {
      continue;
    }
    sum_mhz += *parsed;
    ++count;
  }
  if (count <= 0) {
    return 0.0;
  }
  return sum_mhz / static_cast<double>(count);
#else
  return 0.0;
#endif
}

json ToNullableJson(const std::optional<double>& value) {
  if (!value.has_value()) {
    return nullptr;
  }
  return *value;
}

#if defined(__APPLE__)
uint64_t ReadSysctlUint64(const char* key) {
  if (key == nullptr || key[0] == '\0') {
    return 0;
  }

  uint64_t value = 0;
  size_t size = sizeof(value);
  if (::sysctlbyname(key, &value, &size, nullptr, 0) == 0 && size == sizeof(value)) {
    return value;
  }

  uint32_t value32 = 0;
  size = sizeof(value32);
  if (::sysctlbyname(key, &value32, &size, nullptr, 0) == 0 && size == sizeof(value32)) {
    return static_cast<uint64_t>(value32);
  }
  return 0;
}

std::optional<double> ParseMacGpuUtilization(const std::string& ioreg_dump) {
  static const std::regex kDevicePattern(R"("Device Utilization %"\s*=\s*([0-9]+(?:\.[0-9]+)?))");
  static const std::regex kRendererPattern(R"("Renderer Utilization %"\s*=\s*([0-9]+(?:\.[0-9]+)?))");

  std::smatch match;
  if (std::regex_search(ioreg_dump, match, kDevicePattern) && match.size() >= 2) {
    const auto parsed = ParseDouble(match[1].str());
    if (parsed.has_value()) {
      return *parsed;
    }
  }
  if (std::regex_search(ioreg_dump, match, kRendererPattern) && match.size() >= 2) {
    const auto parsed = ParseDouble(match[1].str());
    if (parsed.has_value()) {
      return *parsed;
    }
  }
  return std::nullopt;
}

std::string ParseMacGpuModel(const std::string& ioreg_dump) {
  static const std::regex kModelPattern(R"regex("model"\s*=\s*"([^"]+)")regex");
  std::smatch match;
  if (std::regex_search(ioreg_dump, match, kModelPattern) && match.size() >= 2) {
    return TrimCopy(match[1].str());
  }
  return "";
}
#endif

}  // namespace

SystemMonitor::SystemMonitor() {
  RefreshStaticInfo();
}

double SystemMonitor::ClampPercent(double value) {
  if (!std::isfinite(value)) {
    return 0.0;
  }
  if (value < 0.0) {
    return 0.0;
  }
  if (value > 100.0) {
    return 100.0;
  }
  return value;
}

double SystemMonitor::CpuUsagePercent(const CpuTimes& previous, const CpuTimes& current) {
  const uint64_t prev_idle = previous.idle + previous.iowait;
  const uint64_t curr_idle = current.idle + current.iowait;
  const uint64_t prev_total =
      previous.user + previous.nice + previous.system + previous.idle + previous.iowait + previous.irq +
      previous.softirq + previous.steal;
  const uint64_t curr_total =
      current.user + current.nice + current.system + current.idle + current.iowait + current.irq +
      current.softirq + current.steal;

  if (curr_total <= prev_total) {
    return 0.0;
  }

  const uint64_t total_delta = curr_total - prev_total;
  const uint64_t idle_delta = curr_idle >= prev_idle ? (curr_idle - prev_idle) : 0;
  if (total_delta == 0) {
    return 0.0;
  }
  const double active = static_cast<double>(total_delta - (std::min)(total_delta, idle_delta));
  return ClampPercent(active * 100.0 / static_cast<double>(total_delta));
}

void SystemMonitor::RefreshStaticInfo() {
  std::lock_guard<std::mutex> lock(mu_);

  hostname_.clear();
  device_model_.clear();
  os_name_.clear();
  architecture_.clear();
  cpu_model_.clear();
  gpu_model_.clear();
  physical_cores_ = 0;
  logical_cores_ = static_cast<int>(std::thread::hardware_concurrency());
  cpu_base_frequency_mhz_ = 0.0;
  total_memory_bytes_ = 0;
  total_disk_bytes_ = 0;

#if defined(_WIN32)
  char host_buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
  DWORD host_len = MAX_COMPUTERNAME_LENGTH + 1;
  if (::GetComputerNameA(host_buf, &host_len) != 0 && host_len > 0) {
    hostname_ = std::string(host_buf, host_len);
  }
  os_name_ = "Windows";
#if defined(_M_ARM64)
  architecture_ = "arm64";
#elif defined(_M_X64)
  architecture_ = "x86_64";
#elif defined(_M_IX86)
  architecture_ = "x86";
#endif
  const char* proc = std::getenv("PROCESSOR_IDENTIFIER");
  if (proc != nullptr) {
    cpu_model_ = TrimCopy(proc);
  }
  device_model_ = hostname_;
#else
  char host_buf[256] = {};
  if (::gethostname(host_buf, sizeof(host_buf) - 1) == 0) {
    hostname_ = TrimCopy(host_buf);
  }
  struct utsname uts {};
  if (::uname(&uts) == 0) {
    architecture_ = TrimCopy(uts.machine);
    os_name_ = TrimCopy(uts.sysname);
    const std::string release = TrimCopy(uts.release);
    if (!release.empty()) {
      os_name_ += " " + release;
    }
  }
#endif

  if (hostname_.empty()) {
    hostname_ = "unknown";
  }
  if (os_name_.empty()) {
    os_name_ = "unknown";
  }
  if (architecture_.empty()) {
    architecture_ = "unknown";
  }

#if defined(__APPLE__)
  const std::string profile = RunCommandCapture("system_profiler SPHardwareDataType 2>/dev/null");
  std::string model_name;
  std::string model_identifier;
  std::string chip_name;
  std::istringstream hardware_profile(profile);
  for (std::string line; std::getline(hardware_profile, line);) {
    const std::string trimmed = TrimCopy(line);
    if (StartsWith(trimmed, "Model Name:")) {
      model_name = TrimCopy(trimmed.substr(std::strlen("Model Name:")));
      continue;
    }
    if (StartsWith(trimmed, "Model Identifier:")) {
      model_identifier = TrimCopy(trimmed.substr(std::strlen("Model Identifier:")));
      continue;
    }
    if (StartsWith(trimmed, "Chip:")) {
      chip_name = TrimCopy(trimmed.substr(std::strlen("Chip:")));
      continue;
    }
    if (StartsWith(trimmed, "Total Number of Cores:")) {
      const int total_cores = ParseFirstInt(trimmed);
      if (total_cores > 0) {
        physical_cores_ = total_cores;
      }
      continue;
    }
  }

  if (!chip_name.empty()) {
    cpu_model_ = chip_name;
  }
  if (!model_name.empty() && !model_identifier.empty()) {
    device_model_ = model_name + " (" + model_identifier + ")";
  } else if (!model_identifier.empty()) {
    device_model_ = model_identifier;
  } else {
    device_model_ = model_name;
  }
  if (device_model_.empty()) {
    device_model_ = architecture_;
  }

  cpu_base_frequency_mhz_ = static_cast<double>(ReadSysctlUint64("hw.cpufrequency")) / 1'000'000.0;
  if (cpu_base_frequency_mhz_ <= 0.0) {
    cpu_base_frequency_mhz_ = static_cast<double>(ReadSysctlUint64("hw.cpufrequency_max")) / 1'000'000.0;
  }

  const std::string ioreg_dump = RunCommandCapture("ioreg -l -w0 -r -c IOAccelerator 2>/dev/null");
  gpu_model_ = ParseMacGpuModel(ioreg_dump);

#elif defined(__linux__)
  const std::string product = ReadFirstLine("/sys/devices/virtual/dmi/id/product_name");
  const std::string vendor = ReadFirstLine("/sys/devices/virtual/dmi/id/sys_vendor");
  device_model_ = TrimCopy(JoinParts({vendor, product}, " "));
  if (device_model_.empty()) {
    device_model_ = ReadFirstLine("/proc/device-tree/model");
  }
  if (device_model_.empty()) {
    device_model_ = architecture_;
  }

  std::ifstream cpuinfo("/proc/cpuinfo");
  if (cpuinfo.is_open()) {
    std::string line;
    while (std::getline(cpuinfo, line)) {
      const std::string trimmed = TrimCopy(line);
      if ((StartsWith(trimmed, "model name") || StartsWith(trimmed, "Hardware") ||
           StartsWith(trimmed, "Processor")) &&
          cpu_model_.empty()) {
        const size_t colon = trimmed.find(':');
        if (colon != std::string::npos && colon + 1 < trimmed.size()) {
          cpu_model_ = TrimCopy(trimmed.substr(colon + 1));
        }
      }
      if (StartsWith(trimmed, "cpu cores") && physical_cores_ <= 0) {
        physical_cores_ = ParseFirstInt(trimmed);
      }
    }
  }

  cpu_base_frequency_mhz_ = LinuxCpuFrequencyMhz();

  const std::string gpu_name = FirstNonEmptyLine(
      RunCommandCapture("nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null"));
  gpu_model_ = gpu_name;
#else
  if (device_model_.empty()) {
    device_model_ = hostname_;
  }
#endif

  uint64_t mem_total = 0;
  uint64_t mem_used = 0;
  uint64_t mem_free = 0;
  if (ReadMemoryStats(&mem_total, &mem_used, &mem_free)) {
    total_memory_bytes_ = mem_total;
  }

  uint64_t disk_total = 0;
  uint64_t disk_used = 0;
  uint64_t disk_free = 0;
  if (ReadDiskStats(&disk_total, &disk_used, &disk_free)) {
    total_disk_bytes_ = disk_total;
  }
}

bool SystemMonitor::ReadCpuTimes(std::vector<CpuTimes>* samples, double* frequency_mhz) const {
  if (samples == nullptr || frequency_mhz == nullptr) {
    return false;
  }
  samples->clear();
  *frequency_mhz = 0.0;

#if defined(__APPLE__)
  natural_t processor_count = 0;
  processor_info_array_t cpu_info = nullptr;
  mach_msg_type_number_t info_count = 0;
  const kern_return_t status =
      ::host_processor_info(::mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &processor_count, &cpu_info, &info_count);
  if (status != KERN_SUCCESS || cpu_info == nullptr || processor_count == 0) {
    return false;
  }

  std::vector<CpuTimes> next;
  next.reserve(static_cast<size_t>(processor_count) + 1);
  CpuTimes aggregate;
  next.push_back(aggregate);
  for (natural_t cpu = 0; cpu < processor_count; ++cpu) {
    const size_t offset = static_cast<size_t>(cpu) * CPU_STATE_MAX;
    CpuTimes item;
    item.user = static_cast<uint64_t>(cpu_info[offset + CPU_STATE_USER]);
    item.system = static_cast<uint64_t>(cpu_info[offset + CPU_STATE_SYSTEM]);
    item.idle = static_cast<uint64_t>(cpu_info[offset + CPU_STATE_IDLE]);
    item.nice = static_cast<uint64_t>(cpu_info[offset + CPU_STATE_NICE]);
    aggregate.user += item.user;
    aggregate.system += item.system;
    aggregate.idle += item.idle;
    aggregate.nice += item.nice;
    next.push_back(item);
  }
  next[0] = aggregate;

  (void)::vm_deallocate(::mach_task_self(), reinterpret_cast<vm_address_t>(cpu_info),
                        static_cast<vm_size_t>(info_count * sizeof(integer_t)));

  *samples = std::move(next);
  const double base = static_cast<double>(ReadSysctlUint64("hw.cpufrequency")) / 1'000'000.0;
  *frequency_mhz = base > 0.0 ? base : static_cast<double>(ReadSysctlUint64("hw.cpufrequency_max")) / 1'000'000.0;
  return true;
#elif defined(__linux__)
  std::ifstream file("/proc/stat");
  if (!file.is_open()) {
    return false;
  }

  std::vector<CpuTimes> next;
  std::string line;
  while (std::getline(file, line)) {
    if (!StartsWith(line, "cpu")) {
      break;
    }

    std::istringstream in(line);
    std::string label;
    in >> label;
    if (label != "cpu") {
      if (label.size() <= 3 || !std::all_of(label.begin() + 3, label.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
          })) {
        continue;
      }
    }

    CpuTimes item;
    in >> item.user >> item.nice >> item.system >> item.idle >> item.iowait >> item.irq >> item.softirq >>
        item.steal;
    if (in.fail()) {
      continue;
    }
    next.push_back(item);
  }

  if (next.empty()) {
    return false;
  }
  *samples = std::move(next);
  *frequency_mhz = LinuxCpuFrequencyMhz();
  return true;
#elif defined(_WIN32)
  FILETIME idle_ft {};
  FILETIME kernel_ft {};
  FILETIME user_ft {};
  if (::GetSystemTimes(&idle_ft, &kernel_ft, &user_ft) == 0) {
    return false;
  }

  const auto to_uint64 = [](const FILETIME& ft) -> uint64_t {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | static_cast<uint64_t>(ft.dwLowDateTime);
  };

  CpuTimes aggregate;
  aggregate.user = to_uint64(user_ft);
  const uint64_t kernel_total = to_uint64(kernel_ft);
  aggregate.idle = to_uint64(idle_ft);
  aggregate.system = kernel_total > aggregate.idle ? (kernel_total - aggregate.idle) : 0;

  SYSTEM_INFO sys_info {};
  ::GetSystemInfo(&sys_info);
  const int logical = std::max<int>(1, static_cast<int>(sys_info.dwNumberOfProcessors));

  samples->reserve(static_cast<size_t>(logical) + 1);
  samples->push_back(aggregate);
  for (int idx = 0; idx < logical; ++idx) {
    samples->push_back(aggregate);
  }
  return true;
#else
  return false;
#endif
}

bool SystemMonitor::ReadMemoryStats(uint64_t* total_bytes, uint64_t* used_bytes, uint64_t* free_bytes) const {
  if (total_bytes == nullptr || used_bytes == nullptr || free_bytes == nullptr) {
    return false;
  }
  *total_bytes = 0;
  *used_bytes = 0;
  *free_bytes = 0;

#if defined(__APPLE__)
  const uint64_t total = ReadSysctlUint64("hw.memsize");
  if (total == 0) {
    return false;
  }

  mach_port_t host = ::mach_host_self();
  vm_size_t page_size = 0;
  if (::host_page_size(host, &page_size) != KERN_SUCCESS) {
    return false;
  }

  vm_statistics64_data_t vm_stats {};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (::host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm_stats), &count) != KERN_SUCCESS) {
    return false;
  }

  const uint64_t free_estimate =
      static_cast<uint64_t>(vm_stats.free_count + vm_stats.inactive_count) * static_cast<uint64_t>(page_size);
  const uint64_t used_estimate = total > free_estimate ? (total - free_estimate) : 0;

  *total_bytes = total;
  *used_bytes = (std::min)(total, used_estimate);
  *free_bytes = total - *used_bytes;
  return true;
#elif defined(__linux__)
  std::ifstream file("/proc/meminfo");
  if (!file.is_open()) {
    return false;
  }

  uint64_t mem_total_kb = 0;
  uint64_t mem_available_kb = 0;
  uint64_t mem_free_kb = 0;
  uint64_t buffers_kb = 0;
  uint64_t cached_kb = 0;

  std::string key;
  uint64_t value = 0;
  std::string unit;
  while (file >> key >> value >> unit) {
    if (key == "MemTotal:") {
      mem_total_kb = value;
    } else if (key == "MemAvailable:") {
      mem_available_kb = value;
    } else if (key == "MemFree:") {
      mem_free_kb = value;
    } else if (key == "Buffers:") {
      buffers_kb = value;
    } else if (key == "Cached:") {
      cached_kb = value;
    }
  }

  if (mem_total_kb == 0) {
    return false;
  }
  if (mem_available_kb == 0) {
    mem_available_kb = mem_free_kb + buffers_kb + cached_kb;
  }
  if (mem_available_kb > mem_total_kb) {
    mem_available_kb = mem_total_kb;
  }

  const uint64_t total = mem_total_kb * 1024ULL;
  const uint64_t free = mem_available_kb * 1024ULL;
  const uint64_t used = total > free ? (total - free) : 0;
  *total_bytes = total;
  *used_bytes = used;
  *free_bytes = total - used;
  return true;
#elif defined(_WIN32)
  MEMORYSTATUSEX memory_status {};
  memory_status.dwLength = sizeof(memory_status);
  if (::GlobalMemoryStatusEx(&memory_status) == 0) {
    return false;
  }
  *total_bytes = static_cast<uint64_t>(memory_status.ullTotalPhys);
  *free_bytes = static_cast<uint64_t>(memory_status.ullAvailPhys);
  *used_bytes = *total_bytes > *free_bytes ? (*total_bytes - *free_bytes) : 0;
  return true;
#else
  return false;
#endif
}

bool SystemMonitor::ReadDiskStats(uint64_t* total_bytes, uint64_t* used_bytes, uint64_t* free_bytes) const {
  if (total_bytes == nullptr || used_bytes == nullptr || free_bytes == nullptr) {
    return false;
  }
  *total_bytes = 0;
  *used_bytes = 0;
  *free_bytes = 0;

#if defined(_WIN32)
  std::filesystem::path root = std::filesystem::current_path().root_path();
  if (root.empty()) {
    root = "C:\\";
  }
#else
  const std::filesystem::path root = "/";
#endif

  std::error_code ec;
  const auto space = std::filesystem::space(root, ec);
  if (ec || space.capacity == 0) {
    return false;
  }

  *total_bytes = static_cast<uint64_t>(space.capacity);
  *free_bytes = static_cast<uint64_t>(space.available);
  *used_bytes = *total_bytes > *free_bytes ? (*total_bytes - *free_bytes) : 0;
  return true;
}

uint64_t SystemMonitor::ReadUptimeSeconds() const {
#if defined(__APPLE__)
  struct timespec ts {};
#if defined(CLOCK_UPTIME_RAW)
  if (::clock_gettime(CLOCK_UPTIME_RAW, &ts) == 0) {
    return static_cast<uint64_t>(ts.tv_sec);
  }
#endif
  if (::clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    return static_cast<uint64_t>(ts.tv_sec);
  }
  return 0;
#elif defined(__linux__)
  struct sysinfo info {};
  if (::sysinfo(&info) == 0 && info.uptime > 0) {
    return static_cast<uint64_t>(info.uptime);
  }
  std::ifstream file("/proc/uptime");
  double uptime = 0.0;
  if (file.is_open()) {
    file >> uptime;
  }
  if (!std::isfinite(uptime) || uptime < 0.0) {
    return 0;
  }
  return static_cast<uint64_t>(uptime);
#elif defined(_WIN32)
  return static_cast<uint64_t>(::GetTickCount64() / 1000ULL);
#else
  return 0;
#endif
}

std::optional<double> SystemMonitor::ReadGpuLoadPercent(std::string* detected_model) const {
#if defined(__APPLE__)
  const std::string ioreg_dump = RunCommandCapture("ioreg -l -w0 -r -c IOAccelerator 2>/dev/null");
  if (detected_model != nullptr) {
    const std::string model = ParseMacGpuModel(ioreg_dump);
    if (!model.empty()) {
      *detected_model = model;
    }
  }

  auto utilization = ParseMacGpuUtilization(ioreg_dump);
  if (!utilization.has_value()) {
    return std::nullopt;
  }
  double normalized = *utilization;
  if (normalized > 1000.0) {
    normalized /= 100.0;
  }
  return ClampPercent(normalized);
#elif defined(__linux__)
  const std::string smi_line =
      FirstNonEmptyLine(RunCommandCapture(
          "nvidia-smi --query-gpu=name,utilization.gpu --format=csv,noheader,nounits 2>/dev/null"));
  if (!smi_line.empty()) {
    const size_t comma = smi_line.find(',');
    if (comma != std::string::npos) {
      const std::string name = TrimCopy(smi_line.substr(0, comma));
      std::string util_raw = TrimCopy(smi_line.substr(comma + 1));
      util_raw.erase(std::remove(util_raw.begin(), util_raw.end(), '%'), util_raw.end());
      if (detected_model != nullptr && !name.empty()) {
        *detected_model = name;
      }
      const auto parsed = ParseDouble(util_raw);
      if (parsed.has_value()) {
        return ClampPercent(*parsed);
      }
    }
  }

  const std::string drm_busy = ReadFirstLine("/sys/class/drm/card0/device/gpu_busy_percent");
  const auto parsed = ParseDouble(drm_busy);
  if (parsed.has_value()) {
    return ClampPercent(*parsed);
  }
  return std::nullopt;
#else
  (void)detected_model;
  return std::nullopt;
#endif
}

std::string SystemMonitor::SnapshotJson() {
  std::vector<CpuTimes> cpu_samples;
  double cpu_frequency_mhz = 0.0;
  const bool has_cpu_samples = ReadCpuTimes(&cpu_samples, &cpu_frequency_mhz);

  std::string hostname;
  std::string device_model;
  std::string os_name;
  std::string architecture;
  std::string cpu_model;
  std::string gpu_model;
  int logical_cores = 0;
  int physical_cores = 0;
  double cpu_base_frequency_mhz = 0.0;
  uint64_t total_memory_static = 0;
  uint64_t total_disk_static = 0;

  std::vector<double> per_core_load_percent;
  double total_cpu_load_percent = 0.0;

  {
    std::lock_guard<std::mutex> lock(mu_);
    hostname = hostname_;
    device_model = device_model_;
    os_name = os_name_;
    architecture = architecture_;
    cpu_model = cpu_model_;
    gpu_model = gpu_model_;
    logical_cores = logical_cores_;
    physical_cores = physical_cores_;
    cpu_base_frequency_mhz = cpu_base_frequency_mhz_;
    total_memory_static = total_memory_bytes_;
    total_disk_static = total_disk_bytes_;

    if (has_cpu_samples && !cpu_samples.empty()) {
      if (cpu_times_initialized_ && last_cpu_times_.size() == cpu_samples.size()) {
        total_cpu_load_percent = CpuUsagePercent(last_cpu_times_[0], cpu_samples[0]);
        per_core_load_percent.reserve(cpu_samples.size() > 1 ? cpu_samples.size() - 1 : 0);
        for (size_t idx = 1; idx < cpu_samples.size(); ++idx) {
          per_core_load_percent.push_back(CpuUsagePercent(last_cpu_times_[idx], cpu_samples[idx]));
        }
      } else if (cpu_samples.size() > 1) {
        per_core_load_percent.assign(cpu_samples.size() - 1, 0.0);
      }
      last_cpu_times_ = cpu_samples;
      cpu_times_initialized_ = true;
    } else if (logical_cores > 0) {
      per_core_load_percent.assign(static_cast<size_t>(logical_cores), 0.0);
    }
  }

  if (logical_cores <= 0 && !per_core_load_percent.empty()) {
    logical_cores = static_cast<int>(per_core_load_percent.size());
  }
  if (physical_cores <= 0) {
    physical_cores = logical_cores;
  }
  if (logical_cores > 0 && per_core_load_percent.empty()) {
    per_core_load_percent.assign(static_cast<size_t>(logical_cores), 0.0);
  }
  if (cpu_frequency_mhz <= 0.0) {
    cpu_frequency_mhz = cpu_base_frequency_mhz;
  }

  uint64_t memory_total_bytes = 0;
  uint64_t memory_used_bytes = 0;
  uint64_t memory_free_bytes = 0;
  if (!ReadMemoryStats(&memory_total_bytes, &memory_used_bytes, &memory_free_bytes)) {
    memory_total_bytes = total_memory_static;
  }
  if (memory_total_bytes == 0) {
    memory_total_bytes = total_memory_static;
  }

  uint64_t disk_total_bytes = 0;
  uint64_t disk_used_bytes = 0;
  uint64_t disk_free_bytes = 0;
  if (!ReadDiskStats(&disk_total_bytes, &disk_used_bytes, &disk_free_bytes)) {
    disk_total_bytes = total_disk_static;
  }
  if (disk_total_bytes == 0) {
    disk_total_bytes = total_disk_static;
  }

  const uint64_t uptime_seconds = ReadUptimeSeconds();
  const int64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const int64_t started_at_ms = uptime_seconds > 0
      ? (now_ms - static_cast<int64_t>(uptime_seconds) * 1000LL)
      : 0;

  std::string detected_gpu_model = gpu_model;
  const std::optional<double> gpu_load_percent = ReadGpuLoadPercent(&detected_gpu_model);
  if (gpu_model.empty() && !detected_gpu_model.empty()) {
    std::lock_guard<std::mutex> lock(mu_);
    gpu_model_ = detected_gpu_model;
    gpu_model = detected_gpu_model;
  } else if (!detected_gpu_model.empty()) {
    gpu_model = detected_gpu_model;
  }

  const double memory_used_percent = memory_total_bytes > 0
      ? ClampPercent(static_cast<double>(memory_used_bytes) * 100.0 / static_cast<double>(memory_total_bytes))
      : 0.0;
  const double disk_used_percent = disk_total_bytes > 0
      ? ClampPercent(static_cast<double>(disk_used_bytes) * 100.0 / static_cast<double>(disk_total_bytes))
      : 0.0;

  std::vector<std::string> configuration_parts;
  if (!cpu_model.empty()) {
    configuration_parts.push_back(cpu_model);
  }
  if (logical_cores > 0) {
    configuration_parts.push_back(std::to_string(logical_cores) + " cores");
  }
  if (memory_total_bytes > 0) {
    configuration_parts.push_back(FormatBytesCompact(memory_total_bytes) + " RAM");
  }
  if (disk_total_bytes > 0) {
    configuration_parts.push_back(FormatBytesCompact(disk_total_bytes) + " disk");
  }
  const std::string configuration = JoinParts(configuration_parts, " / ");

  json snapshot;
  snapshot["ts_ms"] = now_ms;
  snapshot["device"] = {
      {"name", hostname},
      {"model", device_model},
      {"configuration", configuration},
      {"os", os_name},
      {"architecture", architecture},
      {"cpu_model", cpu_model},
      {"gpu_model", gpu_model},
      {"logical_cores", logical_cores},
      {"physical_cores", physical_cores},
  };
  snapshot["cpu"] = {
      {"base_frequency_mhz", cpu_base_frequency_mhz},
      {"frequency_mhz", cpu_frequency_mhz},
      {"total_load_percent", total_cpu_load_percent},
      {"per_core_load_percent", per_core_load_percent},
  };
  snapshot["gpu"] = {
      {"model", gpu_model},
      {"load_percent", ToNullableJson(gpu_load_percent)},
  };
  snapshot["boot"] = {
      {"uptime_seconds", uptime_seconds},
      {"started_at_ms", started_at_ms},
  };
  snapshot["memory"] = {
      {"total_bytes", memory_total_bytes},
      {"used_bytes", memory_used_bytes},
      {"free_bytes", memory_free_bytes},
      {"used_percent", memory_used_percent},
  };
  snapshot["disk"] = {
      {"total_bytes", disk_total_bytes},
      {"used_bytes", disk_used_bytes},
      {"free_bytes", disk_free_bytes},
      {"used_percent", disk_used_percent},
  };
  return snapshot.dump();
}

}  // namespace ferryman::web
