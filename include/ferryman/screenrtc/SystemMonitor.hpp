#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ferryman::web {

class SystemMonitor {
 public:
  SystemMonitor();

  std::string SnapshotJson();

 private:
  struct CpuTimes {
    uint64_t user = 0;
    uint64_t nice = 0;
    uint64_t system = 0;
    uint64_t idle = 0;
    uint64_t iowait = 0;
    uint64_t irq = 0;
    uint64_t softirq = 0;
    uint64_t steal = 0;
  };

  struct DiskVolume {
    std::string id;
    std::string name;
    std::string mount;
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    uint64_t free_bytes = 0;
    double used_percent = 0.0;
  };

  static double ClampPercent(double value);
  static double CpuUsagePercent(const CpuTimes& previous, const CpuTimes& current);

  void RefreshStaticInfo();
  bool ReadCpuTimes(std::vector<CpuTimes>* samples, double* frequency_mhz) const;
  bool ReadMemoryStats(uint64_t* total_bytes, uint64_t* used_bytes, uint64_t* free_bytes) const;
  bool ReadDiskVolumes(std::vector<DiskVolume>* volumes) const;
  bool ReadDiskStats(uint64_t* total_bytes, uint64_t* used_bytes, uint64_t* free_bytes) const;
  uint64_t ReadUptimeSeconds() const;
  std::optional<double> ReadGpuLoadPercent(std::string* detected_model) const;

  mutable std::mutex mu_;
  std::vector<CpuTimes> last_cpu_times_;
  bool cpu_times_initialized_ = false;

  std::string hostname_;
  std::string device_model_;
  std::string os_name_;
  std::string architecture_;
  std::string cpu_model_;
  std::string gpu_model_;
  int logical_cores_ = 0;
  int physical_cores_ = 0;
  double cpu_base_frequency_mhz_ = 0.0;
  uint64_t total_memory_bytes_ = 0;
  uint64_t total_disk_bytes_ = 0;
};

}  // namespace ferryman::web
