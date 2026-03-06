#include "CodeAgentPolicy.hpp"

#include "ferryman/util/StringUtil.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace ferryman::codeagent::policy {

namespace {

template <size_t N>
bool ArrayContains(const std::array<std::string_view, N>& values, std::string_view target) {
  return std::find(values.begin(), values.end(), target) != values.end();
}

}  // namespace

std::string NormalizePermissionModeValue(std::string mode) {
  mode = util::Trim(mode);
  if (mode.empty()) {
    return "default";
  }

  std::string collapsed;
  collapsed.reserve(mode.size());
  for (char ch : mode) {
    if (std::isalnum(static_cast<unsigned char>(ch)) == 0) {
      continue;
    }
    collapsed.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }

  if (collapsed == "default") {
    return "default";
  }
  if (collapsed == "acceptedits") {
    return "acceptEdits";
  }
  if (collapsed == "bypasspermissions") {
    return "bypassPermissions";
  }
  if (collapsed == "plan") {
    return "plan";
  }
  if (collapsed == "readonly") {
    return "read-only";
  }
  if (collapsed == "safeyolo") {
    return "safe-yolo";
  }
  if (collapsed == "yolo") {
    return "yolo";
  }
  if (collapsed == "ask") {
    return "ask";
  }
  return "";
}

bool IsCodexOrGeminiFlavor(std::string_view flavor) {
  return flavor == "codex" || flavor == "gemini";
}

bool IsPermissionModeAllowedForFlavor(std::string_view mode, std::string_view flavor) {
  static constexpr std::array<std::string_view, 4> kClaudeModes = {"default", "acceptEdits", "bypassPermissions",
                                                                    "plan"};
  static constexpr std::array<std::string_view, 4> kCodexGeminiModes = {"default", "read-only", "safe-yolo", "yolo"};
  static constexpr std::array<std::string_view, 2> kOpencodeModes = {"default", "yolo"};
  static constexpr std::array<std::string_view, 4> kCursorModes = {"default", "plan", "ask", "yolo"};

  if (IsCodexOrGeminiFlavor(flavor)) {
    return ArrayContains(kCodexGeminiModes, mode);
  }
  if (flavor == "opencode") {
    return ArrayContains(kOpencodeModes, mode);
  }
  if (flavor == "cursor") {
    return ArrayContains(kCursorModes, mode);
  }
  return ArrayContains(kClaudeModes, mode);
}

std::string ResolveSpawnPermissionMode(std::string_view flavor, bool yolo) {
  if (!yolo) {
    return "default";
  }
  if (flavor == "claude") {
    return "bypassPermissions";
  }
  if (IsCodexOrGeminiFlavor(flavor) || flavor == "cursor" || flavor == "opencode") {
    return "yolo";
  }
  return "default";
}

bool IsKnownModelMode(std::string_view mode) {
  static constexpr std::array<std::string_view, 3> kModelModes = {"default", "sonnet", "opus"};
  return ArrayContains(kModelModes, mode);
}

bool IsModelModeAllowedForFlavor(std::string_view mode, std::string_view flavor) {
  if (!IsKnownModelMode(mode)) {
    return false;
  }
  return flavor == "claude";
}

std::string NormalizeReasoningEffortValue(std::string effort) {
  effort = util::Trim(effort);
  std::transform(effort.begin(), effort.end(), effort.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  std::replace(effort.begin(), effort.end(), '_', '-');
  effort.erase(
      std::remove_if(effort.begin(), effort.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
      effort.end());
  if (effort == "default") {
    return "";
  }
  return effort;
}

bool IsKnownReasoningEffort(std::string_view effort) {
  static constexpr std::array<std::string_view, 4> kReasoningEfforts = {"low", "medium", "high", "xhigh"};
  return ArrayContains(kReasoningEfforts, effort);
}

bool IsReasoningEffortAllowedForFlavor(std::string_view effort, std::string_view flavor) {
  if (effort.empty()) {
    return true;
  }
  if (!IsKnownReasoningEffort(effort)) {
    return false;
  }
  return flavor == "codex";
}

}  // namespace ferryman::codeagent::policy
