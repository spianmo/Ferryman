#include "ferryman/util/Time.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace ferryman::util {

std::string UtcNowIso8601() {
  const auto now = std::chrono::system_clock::now();
  const auto tt = std::chrono::system_clock::to_time_t(now);
#if defined(_WIN32)
  std::tm tm{};
  gmtime_s(&tm, &tt);
#else
  std::tm tm{};
  gmtime_r(&tt, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

int64_t UtcNowEpochSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace ferryman::util
