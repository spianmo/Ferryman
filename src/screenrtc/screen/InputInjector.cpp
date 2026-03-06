#include "ferryman/screenrtc/ScreenService.hpp"

#include <mutex>
#include <string>

namespace ferryman::web {

bool ScreenService::SetRemoteControlEnabled(const std::string& session_token, bool enabled) {
  std::lock_guard<std::mutex> lock(mu_);
  control_enabled_[session_token] = enabled;
  return true;
}

bool ScreenService::CanInjectInput(const std::string& session_token) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = control_enabled_.find(session_token);
  if (it == control_enabled_.end()) {
    return true;
  }
  return it->second;
}

bool ScreenService::InjectInputEvent(const std::string& session_token, const InputEvent& event, std::string* error) {
  if (!CanInjectInput(session_token)) {
    if (error != nullptr) {
      *error = "remote control is not authorized";
    }
    return false;
  }
  return InjectInputEventNative(event, error);
}

}  // namespace ferryman::web
