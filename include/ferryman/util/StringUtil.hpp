#pragma once

#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace ferryman::util {

struct JsonField {
  std::string key;
  std::string value;
  bool raw_value = false;
};

std::string Trim(std::string_view value);
std::string JsonEscape(std::string_view value);
std::string Base64Encode(std::string_view data);
std::string Base64Decode(std::string_view encoded);
std::unordered_map<std::string, std::string> ParseFlatJsonObject(std::string_view json);
std::string BuildJsonObject(const std::vector<JsonField>& fields);

}  // namespace ferryman::util
