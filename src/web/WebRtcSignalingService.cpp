#include "ferryman/web/WebRtcSignalingService.hpp"

#include "ferryman/util/Random.hpp"

namespace ferryman::web {

std::optional<SignalingPeer> WebRtcSignalingService::JoinRoom(std::uintptr_t channel_key,
                                                              const std::string& session_token,
                                                              const std::string& room_id) {
  if (room_id.empty()) {
    return std::nullopt;
  }

  std::lock_guard<std::mutex> lock(mu_);

  auto existing = peers_by_channel_.find(channel_key);
  if (existing != peers_by_channel_.end()) {
    auto room_it = channels_by_room_.find(existing->second.room_id);
    if (room_it != channels_by_room_.end()) {
      room_it->second.erase(channel_key);
      if (room_it->second.empty()) {
        channels_by_room_.erase(room_it);
      }
    }
    channel_by_peer_id_.erase(existing->second.peer_id);
    peers_by_channel_.erase(existing);
  }

  SignalingPeer peer;
  peer.peer_id = util::RandomHex(10);
  peer.room_id = room_id;
  peer.session_token = session_token;

  peers_by_channel_[channel_key] = peer;
  channels_by_room_[room_id].insert(channel_key);
  channel_by_peer_id_[peer.peer_id] = channel_key;
  return peer;
}

void WebRtcSignalingService::Leave(std::uintptr_t channel_key) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = peers_by_channel_.find(channel_key);
  if (it == peers_by_channel_.end()) {
    return;
  }

  channel_by_peer_id_.erase(it->second.peer_id);
  auto room_it = channels_by_room_.find(it->second.room_id);
  if (room_it != channels_by_room_.end()) {
    room_it->second.erase(channel_key);
    if (room_it->second.empty()) {
      channels_by_room_.erase(room_it);
    }
  }
  peers_by_channel_.erase(it);
}

std::optional<SignalingPeer> WebRtcSignalingService::GetPeer(std::uintptr_t channel_key) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = peers_by_channel_.find(channel_key);
  if (it == peers_by_channel_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<SignalingPeer> WebRtcSignalingService::PeersInRoom(const std::string& room_id) const {
  std::vector<SignalingPeer> peers;
  std::lock_guard<std::mutex> lock(mu_);
  auto room_it = channels_by_room_.find(room_id);
  if (room_it == channels_by_room_.end()) {
    return peers;
  }

  peers.reserve(room_it->second.size());
  for (const auto& channel_key : room_it->second) {
    auto peer_it = peers_by_channel_.find(channel_key);
    if (peer_it != peers_by_channel_.end()) {
      peers.push_back(peer_it->second);
    }
  }
  return peers;
}

std::optional<std::uintptr_t> WebRtcSignalingService::FindChannelByPeerId(const std::string& peer_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = channel_by_peer_id_.find(peer_id);
  if (it == channel_by_peer_id_.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace ferryman::web
