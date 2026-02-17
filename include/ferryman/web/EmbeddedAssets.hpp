#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ferryman::web {

struct EmbeddedAsset {
  std::string_view mime_type;
  std::string_view content;
};

const std::unordered_map<std::string, EmbeddedAsset>& GetEmbeddedAssets();
std::optional<EmbeddedAsset> FindEmbeddedAsset(const std::string& path);

}  // namespace ferryman::web
