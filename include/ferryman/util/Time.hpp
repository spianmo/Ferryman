#pragma once

#include <chrono>
#include <string>

namespace ferryman::util {

std::string UtcNowIso8601();
int64_t UtcNowEpochSeconds();

}  // namespace ferryman::util
