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

  static constexpr std::array<std::string_view, 14> kKnownModes = {
      "default",
      "acceptEdits",
      "plan",
      "bypassPermissions",
      "ask",
      "read-only",
      "auto",
      "full-access",
      "auto-edit",
      "yolo",
      "allow",
      "deny",
      "agent",
      "force",
  };

  return ArrayContains(kKnownModes, mode) ? mode : std::string();
}

std::string CanonicalizePermissionModeForFlavor(std::string mode, std::string_view flavor) {
  const std::string normalized = NormalizePermissionModeValue(std::move(mode));
  if (normalized.empty()) {
    return "";
  }

  static constexpr std::array<std::string_view, 4> kClaudeModes = {"default", "acceptEdits", "plan",
                                                                    "bypassPermissions"};
  static constexpr std::array<std::string_view, 3> kCodexModes = {"read-only", "auto", "full-access"};
  static constexpr std::array<std::string_view, 4> kGeminiModes = {"default", "auto-edit", "plan", "yolo"};
  static constexpr std::array<std::string_view, 3> kOpencodeModes = {"ask", "allow", "deny"};
  static constexpr std::array<std::string_view, 4> kCursorModes = {"agent", "plan", "ask", "force"};

  if (flavor == "codex") {
    return ArrayContains(kCodexModes, normalized) ? normalized : std::string();
  }
  if (flavor == "gemini") {
    return ArrayContains(kGeminiModes, normalized) ? normalized : std::string();
  }
  if (flavor == "opencode") {
    return ArrayContains(kOpencodeModes, normalized) ? normalized : std::string();
  }
  if (flavor == "cursor") {
    return ArrayContains(kCursorModes, normalized) ? normalized : std::string();
  }
  return ArrayContains(kClaudeModes, normalized) ? normalized : std::string();
}

std::string DefaultPermissionModeForFlavor(std::string_view flavor) {
  if (flavor == "codex") {
    return "auto";
  }
  if (flavor == "cursor") {
    return "agent";
  }
  if (flavor == "opencode") {
    return "ask";
  }
  return "default";
}

bool IsCodexOrGeminiFlavor(std::string_view flavor) {
  return flavor == "codex" || flavor == "gemini";
}

bool IsPermissionModeAllowedForFlavor(std::string_view mode, std::string_view flavor) {
  return !CanonicalizePermissionModeForFlavor(std::string(mode), flavor).empty();
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
