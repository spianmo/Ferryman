#pragma once

#include <random>
#include <string>

namespace ferryman::util {

inline std::string RandomHex(size_t length) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  static constexpr char kHex[] = "0123456789abcdef";
  std::uniform_int_distribution<int> dist(0, 15);
  std::string out;
  out.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    out.push_back(kHex[dist(rng)]);
  }
  return out;
}

}  // namespace ferryman::util
