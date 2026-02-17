#include "ferryman/web/EmbeddedAssets.hpp"

namespace ferryman::web {

std::optional<EmbeddedAsset> FindEmbeddedAsset(const std::string& path) {
  const auto& assets = GetEmbeddedAssets();
  std::string normalized = path.empty() ? "/" : path;

  auto it = assets.find(normalized);
  if (it != assets.end()) {
    return it->second;
  }

  if (!normalized.empty() && normalized.back() == '/') {
    const std::string with_index = normalized + "index.html";
    it = assets.find(with_index);
    if (it != assets.end()) {
      return it->second;
    }
  }

  it = assets.find("/index.html");
  if (it != assets.end()) {
    return it->second;
  }
  return std::nullopt;
}

}  // namespace ferryman::web
