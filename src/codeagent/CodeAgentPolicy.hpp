#pragma once

#include <string>
#include <string_view>

namespace ferryman::codeagent::policy {

std::string NormalizePermissionModeValue(std::string mode);

bool IsCodexOrGeminiFlavor(std::string_view flavor);
bool IsPermissionModeAllowedForFlavor(std::string_view mode, std::string_view flavor);
std::string ResolveSpawnPermissionMode(std::string_view flavor, bool yolo);

bool IsKnownModelMode(std::string_view mode);
bool IsModelModeAllowedForFlavor(std::string_view mode, std::string_view flavor);

std::string NormalizeReasoningEffortValue(std::string effort);
bool IsKnownReasoningEffort(std::string_view effort);
bool IsReasoningEffortAllowedForFlavor(std::string_view effort, std::string_view flavor);

}  // namespace ferryman::codeagent::policy
