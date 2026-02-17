#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ferryman::web {

struct SignalingPeer {
  std::string peer_id;
  std::string room_id;
  std::string session_token;
};

class WebRtcSignalingService {
 public:
  std::optional<SignalingPeer> JoinRoom(std::uintptr_t channel_key, const std::string& session_token,
                                        const std::string& room_id);
  void Leave(std::uintptr_t channel_key);

  std::optional<SignalingPeer> GetPeer(std::uintptr_t channel_key) const;
  std::vector<SignalingPeer> PeersInRoom(const std::string& room_id) const;
  std::optional<std::uintptr_t> FindChannelByPeerId(const std::string& peer_id) const;

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::uintptr_t, SignalingPeer> peers_by_channel_;
  std::unordered_map<std::string, std::unordered_set<std::uintptr_t>> channels_by_room_;
  std::unordered_map<std::string, std::uintptr_t> channel_by_peer_id_;
};

}  // namespace ferryman::web
