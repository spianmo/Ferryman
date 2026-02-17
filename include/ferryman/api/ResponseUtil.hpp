#pragma once

#include "ferryman/util/StringUtil.hpp"

#include <string>
#include <vector>

namespace ferryman::api {

inline std::string Success(const std::vector<util::JsonField>& payload_fields = {}) {
  std::vector<util::JsonField> fields;
  fields.reserve(payload_fields.size() + 1);
  fields.push_back({"ok", "true", true});
  fields.insert(fields.end(), payload_fields.begin(), payload_fields.end());
  return util::BuildJsonObject(fields);
}

inline std::string Error(const std::string& message, const std::string& code = "bad_request") {
  return util::BuildJsonObject({
      {"ok", "false", true},
      {"error", message, false},
      {"code", code, false},
  });
}

}  // namespace ferryman::api
