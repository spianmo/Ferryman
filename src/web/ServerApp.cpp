#include "ferryman/web/ServerApp.hpp"

#include "ferryman/api/ResponseUtil.hpp"
#include "ferryman/util/StringUtil.hpp"
#include "ferryman/web/EmbeddedAssets.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>
#include <vector>

namespace ferryman::web {

namespace {

using nlohmann::json;
constexpr int kNativeCaptureFps = 8;
constexpr int kNativeCaptureMinFps = 1;
constexpr int kNativeCaptureMaxFps = 60;
constexpr int kNativeScaleMinPercent = 40;
constexpr int kNativeScaleMaxPercent = 100;
constexpr int kNativeScaleDefaultPercent = 75;
constexpr int kNativeBitrateMinBps = 500'000;
constexpr int kNativeBitrateMaxBps = 12'000'000;
constexpr int kNativeBitrateDefaultBps = 3'000'000;

constexpr uint32_t kNativeBinaryMagic = 0x314D5246U;  // "FRM1" little-endian
constexpr uint8_t kNativeBinaryCodecJpeg = 1;
constexpr uint8_t kNativeBinaryCodecH264 = 2;
constexpr uint8_t kNativeBinaryCodecH265 = 3;
constexpr uint8_t kNativeBinaryCodecVP8 = 4;
constexpr uint8_t kNativeBinaryCodecVP9 = 5;

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool ParseBool(const std::string& value, bool default_value = false) {
  if (value.empty()) {
    return default_value;
  }
  const std::string lower = ToLower(value);
  return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

int ParseInt(const std::string& value, int default_value) {
  if (value.empty()) {
    return default_value;
  }
  try {
    return std::stoi(value);
  } catch (...) {
    return default_value;
  }
}

std::string JsonArray(const std::vector<std::string>& items) {
  std::ostringstream out;
  out << '[';
  for (size_t i = 0; i < items.size(); ++i) {
    out << items[i];
    if (i + 1 < items.size()) {
      out << ',';
    }
  }
  out << ']';
  return out.str();
}

std::optional<json> ParseJsonOrNull(const std::string& body) {
  if (body.empty()) {
    return std::nullopt;
  }
  json parsed = json::parse(body, nullptr, false);
  if (parsed.is_discarded()) {
    return std::nullopt;
  }
  return parsed;
}

std::string JsonString(const std::optional<json>& payload, const char* key, const std::string& fallback = "") {
  if (!payload.has_value() || !payload->contains(key)) {
    return fallback;
  }
  const auto& value = (*payload)[key];
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<long long>());
  }
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<unsigned long long>());
  }
  if (value.is_number_float()) {
    return std::to_string(value.get<double>());
  }
  if (value.is_boolean()) {
    return value.get<bool>() ? "true" : "false";
  }
  if (value.is_null()) {
    return fallback;
  }
  return value.dump();
}

bool JsonBool(const std::optional<json>& payload, const char* key, bool fallback = false) {
  if (!payload.has_value() || !payload->contains(key)) {
    return fallback;
  }
  const auto& value = (*payload)[key];
  if (value.is_boolean()) {
    return value.get<bool>();
  }
  if (value.is_number_integer()) {
    return value.get<long long>() != 0;
  }
  if (value.is_string()) {
    return ParseBool(value.get<std::string>(), fallback);
  }
  return fallback;
}

int JsonInt(const std::optional<json>& payload, const char* key, int fallback = 0) {
  if (!payload.has_value() || !payload->contains(key)) {
    return fallback;
  }
  const auto& value = (*payload)[key];
  if (value.is_number_integer()) {
    return value.get<int>();
  }
  if (value.is_number_float()) {
    return static_cast<int>(value.get<double>());
  }
  if (value.is_string()) {
    return ParseInt(value.get<std::string>(), fallback);
  }
  return fallback;
}

int ParseNativeScalePercent(const std::string& resolution_tier, int fallback) {
  const std::string tier = ToLower(resolution_tier);
  if (tier == "full") {
    return 100;
  }
  if (tier == "balanced") {
    return 75;
  }
  if (tier == "performance") {
    return 50;
  }
  return std::clamp(fallback, kNativeScaleMinPercent, kNativeScaleMaxPercent);
}

std::string NativeResolutionTierFromScale(int scale_percent) {
  if (scale_percent >= 90) {
    return "full";
  }
  if (scale_percent >= 63) {
    return "balanced";
  }
  return "performance";
}

int ParseNativeBitrateBps(const std::string& bitrate_tier, int fallback) {
  const std::string tier = ToLower(bitrate_tier);
  if (tier == "sd") {
    return 1'500'000;
  }
  if (tier == "hd") {
    return 3'000'000;
  }
  if (tier == "uhd") {
    return 6'000'000;
  }
  return std::clamp(fallback, kNativeBitrateMinBps, kNativeBitrateMaxBps);
}

std::string NativeBitrateTierFromBps(int bitrate_bps) {
  if (bitrate_bps >= 4'500'000) {
    return "uhd";
  }
  if (bitrate_bps >= 2'250'000) {
    return "hd";
  }
  return "sd";
}

void AppendUint32Le(std::string* output, uint32_t value) {
  output->push_back(static_cast<char>(value & 0xFF));
  output->push_back(static_cast<char>((value >> 8) & 0xFF));
  output->push_back(static_cast<char>((value >> 16) & 0xFF));
  output->push_back(static_cast<char>((value >> 24) & 0xFF));
}

void AppendUint64Le(std::string* output, uint64_t value) {
  for (int idx = 0; idx < 8; ++idx) {
    output->push_back(static_cast<char>((value >> (idx * 8)) & 0xFF));
  }
}

std::string BuildNativeBinaryFramePacket(uint8_t codec, bool keyframe, uint64_t sequence, int64_t captured_at_ms,
                                         int width, int height, const std::string& payload_bytes) {
  std::string packet;
  packet.reserve(36 + payload_bytes.size());
  AppendUint32Le(&packet, kNativeBinaryMagic);
  packet.push_back(static_cast<char>(codec));
  packet.push_back(static_cast<char>(keyframe ? 1 : 0));
  packet.push_back('\0');
  packet.push_back('\0');
  AppendUint64Le(&packet, sequence);
  AppendUint64Le(&packet, static_cast<uint64_t>(captured_at_ms));
  AppendUint32Le(&packet, static_cast<uint32_t>(std::max(width, 0)));
  AppendUint32Le(&packet, static_cast<uint32_t>(std::max(height, 0)));
  AppendUint32Le(&packet, static_cast<uint32_t>(payload_bytes.size()));
  packet.append(payload_bytes);
  return packet;
}

}  // namespace

ServerApp::ServerApp(core::AppConfig config)
    : config_(std::move(config)),
      audit_logger_(config_.audit_log_path),
      file_service_(config_.workspace_root) {
#if FERRYMAN_WITH_LIBHV
  audit_logger_.SetRealtimeCallback([this](const std::string& serialized_entry) {
    BroadcastLogEntry(serialized_entry);
  });
#endif
}

ServerApp::~ServerApp() {
  Stop();
}

bool ServerApp::Start() {
#if !FERRYMAN_WITH_LIBHV
  std::cerr << "[ferryman] libhv is not enabled. Reconfigure with FERRYMAN_WITH_LIBHV=ON.\n";
  return false;
#else
  if (running_.exchange(true)) {
    return true;
  }

  // WebSocket and HTTP share one listener.
  config_.ws_port = config_.http_port;

  if (!RegisterHttpRoutes() || !RegisterWsHandlers()) {
    audit_logger_.AppendSystem("error", "server.start", "failed to register HTTP/WS routes");
    running_ = false;
    return false;
  }
  screen_service_.SetEncodingTargets(false, false, false, false, false);

  pty_manager_.SetOutputCallback([this](const std::string& terminal_id, const std::string& chunk) {
    BroadcastTerminalOutput(terminal_id, chunk);
  });

  http_server_.service = &http_service_;
  http_server_.ws = &ws_service_;
  std::snprintf(http_server_.host, sizeof(http_server_.host), "%s", config_.http_host.c_str());
  http_server_.port = config_.http_port;

  http_thread_ = std::thread([this]() {
    http_server_run(&http_server_);
  });

  native_screen_thread_ = std::thread([this]() {
    BroadcastNativeFrames();
  });

  std::cout << "[ferryman] http: http://" << config_.http_host << ':' << config_.http_port << '\n';
  std::cout << "[ferryman] ws:   ws://" << config_.http_host << ':' << config_.ws_port << '\n';
  audit_logger_.AppendSystem("info", "server.start",
                             "http=" + config_.http_host + ":" + std::to_string(config_.http_port) +
                                 ", ws=" + config_.http_host + ":" + std::to_string(config_.ws_port));
  return true;
#endif
}

void ServerApp::Stop() {
  bool was_running = false;
#if FERRYMAN_WITH_LIBHV
  was_running = running_.exchange(false);
  if (was_running) {
    http_server_stop(&http_server_);

    if (http_thread_.joinable()) {
      http_thread_.join();
    }
  }

  if (native_screen_thread_.joinable()) {
    native_screen_thread_.join();
  }
#endif
  screen_service_.StopCapture();
#if FERRYMAN_WITH_LIBHV
  active_capture_fps_ = 0;
  active_capture_scale_percent_ = kNativeScaleDefaultPercent;
  active_capture_video_bitrate_bps_ = kNativeBitrateDefaultBps;
#endif
  pty_manager_.Shutdown();
  if (was_running) {
    audit_logger_.AppendSystem("info", "server.stop", "runtime shutdown completed");
  }
}

bool ServerApp::RegisterHttpRoutes() {
#if !FERRYMAN_WITH_LIBHV
  return false;
#else
  http_service_.GET("/api/health", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleHealth(req, resp);
  });

  http_service_.POST("/api/auth/login", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleLogin(req, resp);
  });

  http_service_.GET("/api/session/me", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleSessionMe(req, resp);
  });

  http_service_.GET("/api/files/list", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleFileList(req, resp);
  });

  http_service_.GET("/api/files/read", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleFileRead(req, resp);
  });

  http_service_.POST("/api/files/write", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleFileWrite(req, resp);
  });

  http_service_.POST("/api/tasks/start", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleTaskStart(req, resp);
  });

  http_service_.GET("/api/tasks/list", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleTaskList(req, resp);
  });

  http_service_.GET("/api/tasks/get", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleTaskGet(req, resp);
  });

  http_service_.GET("/api/logs/tail", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleLogsTail(req, resp);
  });

  http_service_.GET("/api/screen/capabilities", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleScreenCaps(req, resp);
  });

  http_service_.POST("/api/screen/input", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleScreenInput(req, resp);
  });

  http_service_.GET("/", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleStaticAsset(req, resp);
  });

  http_service_.GET("/index.html", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleStaticAsset(req, resp);
  });

  http_service_.GET("/{asset}", [this](HttpRequest* req, HttpResponse* resp) {
    return HandleStaticAsset(req, resp);
  });

  return true;
#endif
}

bool ServerApp::RegisterWsHandlers() {
#if !FERRYMAN_WITH_LIBHV
  return false;
#else
  ws_service_.onopen = [this](const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
    HandleWsOpen(channel, req);
  };

  ws_service_.onmessage = [this](const WebSocketChannelPtr& channel, const std::string& msg) {
    HandleWsMessage(channel, msg);
  };

  ws_service_.onclose = [this](const WebSocketChannelPtr& channel) {
    HandleWsClose(channel);
  };
  return true;
#endif
}

#if FERRYMAN_WITH_LIBHV
int ServerApp::Json(HttpResponse* resp, int status, const std::string& body) const {
  resp->status_code = static_cast<http_status>(status);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  resp->body = body;
  return status;
}

int ServerApp::Text(HttpResponse* resp, int status, const std::string& body,
                    const std::string& content_type) const {
  resp->status_code = static_cast<http_status>(status);
  resp->headers["Content-Type"] = content_type;
  resp->body = body;
  return status;
}

std::string ServerApp::HeaderOf(HttpRequest* req, const std::string& key) const {
  auto it = req->headers.find(key);
  if (it == req->headers.end()) {
    return "";
  }
  return it->second;
}

std::string ServerApp::QueryOf(HttpRequest* req, const std::string& key) const {
  auto it = req->query_params.find(key);
  if (it == req->query_params.end()) {
    return "";
  }
  return it->second;
}

std::optional<core::SessionSnapshot> ServerApp::RequireSession(HttpRequest* req, HttpResponse* resp) {
  std::string token = HeaderOf(req, "X-Session-Token");
  if (token.empty()) {
    token = QueryOf(req, "token");
  }
  if (token.empty()) {
    Json(resp, 401, api::Error("missing session token", "unauthorized"));
    return std::nullopt;
  }

  auto session = session_manager_.GetSession(token);
  if (!session.has_value()) {
    Json(resp, 401, api::Error("invalid session token", "unauthorized"));
    return std::nullopt;
  }
  session_manager_.Touch(token);
  return session;
}

int ServerApp::HandleLogin(HttpRequest* req, HttpResponse* resp) {
  std::string provided = util::Trim(req->body);
  if (!provided.empty() && provided.front() == '{') {
    const auto payload = ParseJsonOrNull(provided);
    provided = JsonString(payload, "access_key", provided);
  }

  if (provided != config_.access_key) {
    return Json(resp, 401, api::Error("invalid access key", "unauthorized"));
  }

  std::string client_ip = req->client_addr.ip;
  if (client_ip.empty()) {
    client_ip = "unknown";
  }

  const std::string token = session_manager_.CreateSession(client_ip);
  audit_logger_.Append(token, "auth.login", "login succeeded");

  return Json(resp, 200, api::Success({
                             {"session_token", token, false},
                             {"ws_port", std::to_string(config_.ws_port), true},
                             {"http_port", std::to_string(config_.http_port), true},
                             {"host", config_.http_host, false},
                         }));
}

int ServerApp::HandleSessionMe(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  return Json(resp, 200,
              api::Success({
                  {"session_id", session->session_id, false},
                  {"client_ip", session->client_ip, false},
                  {"created_at", std::to_string(session->created_at), true},
                  {"last_seen_at", std::to_string(session->last_seen_at), true},
                  {"command_authorized", "true", true},
                  {"screen_authorized", "true", true},
              }));
}

int ServerApp::HandleFileList(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string path = QueryOf(req, "path");
  std::string error;
  const auto entries = file_service_.ListDirectory(path, &error);
  if (!error.empty()) {
    return Json(resp, 400, api::Error(error));
  }

  std::vector<std::string> serialized;
  serialized.reserve(entries.size());
  for (const auto& entry : entries) {
    serialized.push_back(util::BuildJsonObject({
        {"name", entry.name, false},
        {"path", entry.path, false},
        {"is_directory", entry.is_directory ? "true" : "false", true},
        {"size", std::to_string(entry.size), true},
        {"modified_at", std::to_string(entry.modified_at), true},
    }));
  }

  return Json(resp, 200, api::Success({
                             {"entries", JsonArray(serialized), true},
                         }));
}

int ServerApp::HandleFileRead(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string path = QueryOf(req, "path");
  std::string error;
  const auto content = file_service_.ReadFile(path, &error);
  if (!content.has_value()) {
    return Json(resp, 400, api::Error(error.empty() ? "failed to read file" : error));
  }

  audit_logger_.Append(session->token, "file.read", path);
  return Json(resp, 200, api::Success({
                             {"path", path, false},
                             {"content_base64", util::Base64Encode(*content), false},
                         }));
}

int ServerApp::HandleFileWrite(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string path = QueryOf(req, "path");
  const bool base64 = ParseBool(QueryOf(req, "base64"), true);
  const std::string content = base64 ? util::Base64Decode(req->body) : req->body;

  std::string error;
  if (!file_service_.WriteFile(path, content, &error)) {
    return Json(resp, 400, api::Error(error.empty() ? "failed to write file" : error));
  }

  audit_logger_.Append(session->token, "file.write", path);
  return Json(resp, 200, api::Success({
                             {"path", path, false},
                             {"bytes", std::to_string(content.size()), true},
                         }));
}

int ServerApp::HandleTaskStart(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  std::string command = util::Trim(req->body);
  if (!command.empty() && command.front() == '{') {
    const auto payload = ParseJsonOrNull(command);
    command = JsonString(payload, "command", command);
  }

  if (command.empty()) {
    return Json(resp, 400, api::Error("command is required"));
  }

  const std::string task_id = task_manager_.StartTask(session->token, command);
  audit_logger_.Append(session->token, "task.start", command);

  return Json(resp, 200, api::Success({
                             {"task_id", task_id, false},
                         }));
}

int ServerApp::HandleTaskList(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto tasks = task_manager_.ListTasks(session->token);
  std::vector<std::string> serialized;
  serialized.reserve(tasks.size());
  for (const auto& task : tasks) {
    serialized.push_back(util::BuildJsonObject({
        {"task_id", task.task_id, false},
        {"command", task.command, false},
        {"status", task::TaskManager::StatusToString(task.status), false},
        {"exit_code", std::to_string(task.exit_code), true},
        {"created_at", task.created_at, false},
        {"updated_at", task.updated_at, false},
    }));
  }

  return Json(resp, 200, api::Success({
                             {"tasks", JsonArray(serialized), true},
                         }));
}

int ServerApp::HandleTaskGet(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string task_id = QueryOf(req, "task_id");
  auto task = task_manager_.GetTask(session->token, task_id);
  if (!task.has_value()) {
    return Json(resp, 404, api::Error("task not found", "not_found"));
  }

  return Json(resp, 200, api::Success({
                             {"task_id", task->task_id, false},
                             {"command", task->command, false},
                             {"status", task::TaskManager::StatusToString(task->status), false},
                             {"exit_code", std::to_string(task->exit_code), true},
                             {"output_base64", util::Base64Encode(task->output), false},
                             {"created_at", task->created_at, false},
                             {"updated_at", task->updated_at, false},
                         }));
}

int ServerApp::HandleLogsTail(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const int lines = ParseInt(QueryOf(req, "lines"), 200);
  return Json(resp, 200, api::Success({
                             {"items", audit_logger_.Tail(static_cast<size_t>(std::max(1, lines))), true},
                         }));
}

int ServerApp::HandleScreenCaps(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  return Json(resp, 200, api::Success({
                             {"capabilities", screen_service_.CapabilitiesJson(), true},
                             {"screen_authorized", "true", true},
                             {"native_capture_running", screen_service_.IsCapturing() ? "true" : "false", true},
                         }));
}

int ServerApp::HandleScreenInput(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string type = JsonString(payload, "type");
  std::string event_payload = JsonString(payload, "payload");
  if (payload.has_value() && payload->contains("payload") && !(*payload)["payload"].is_string()) {
    event_payload = (*payload)["payload"].dump();
  }
  if (type.empty()) {
    return Json(resp, 400, api::Error("type is required"));
  }

  std::string error;
  if (!screen_service_.InjectInputEvent(session->token, {type, event_payload}, &error)) {
    return Json(resp, 403, api::Error(error, "forbidden"));
  }

  audit_logger_.Append(session->token, "screen.input", type);
  return Json(resp, 200, api::Success({
                             {"accepted", "true", true},
                             {"message", error, false},
                         }));
}

int ServerApp::HandleHealth(HttpRequest* req, HttpResponse* resp) {
  (void)req;
  return Json(resp, 200, api::Success({
                             {"service", "ferryman", false},
                             {"running", running_ ? "true" : "false", true},
                             {"http_port", std::to_string(config_.http_port), true},
                             {"ws_port", std::to_string(config_.ws_port), true},
                         }));
}

int ServerApp::HandleStaticAsset(HttpRequest* req, HttpResponse* resp) {
  if (req->path.rfind("/api/", 0) == 0) {
    return Json(resp, 404, api::Error("not found", "not_found"));
  }

  auto asset = FindEmbeddedAsset(req->path);
  if (!asset.has_value()) {
    return Text(resp, 404, "Not Found");
  }
  return Text(resp, 200, std::string(asset->content), std::string(asset->mime_type));
}

void ServerApp::SendToWs(std::uintptr_t channel_key, const std::string& payload) {
  WebSocketChannelPtr channel;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    auto it = ws_clients_.find(channel_key);
    if (it == ws_clients_.end()) {
      return;
    }
    channel = it->second.channel;
  }
  if (channel) {
    channel->send(payload);
  }
}

ServerApp::NativeCaptureDemand ServerApp::CollectNativeCaptureDemandLocked() const {
  NativeCaptureDemand demand;
  for (const auto& [_, client] : ws_clients_) {
    if (client.channel_type != "webrtc" || !client.native_stream_subscribed || !client.channel) {
      continue;
    }
    demand.subscriber_count += 1;
    demand.fps = std::max(demand.fps, std::clamp(client.native_stream_fps, kNativeCaptureMinFps,
                                                 kNativeCaptureMaxFps));
    if (client.native_stream_codec == "vp9") {
      demand.want_vp9 = true;
    } else if (client.native_stream_codec == "vp8") {
      demand.want_vp8 = true;
    } else if (client.native_stream_codec == "h265") {
      demand.want_h265 = true;
    } else if (client.native_stream_codec == "h264") {
      demand.want_h264 = true;
    } else {
      demand.want_jpeg = true;
    }
    demand.scale_percent =
        std::max(demand.scale_percent, std::clamp(client.native_stream_scale_percent, kNativeScaleMinPercent,
                                                  kNativeScaleMaxPercent));
    demand.video_bitrate_bps =
        std::max(demand.video_bitrate_bps,
                 std::clamp(client.native_stream_video_bitrate_bps, kNativeBitrateMinBps, kNativeBitrateMaxBps));
  }
  return demand;
}

void ServerApp::RefreshNativeCaptureState() {
  NativeCaptureDemand demand;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    demand = CollectNativeCaptureDemandLocked();
  }

  screen_service_.SetEncodingTargets(demand.want_jpeg, demand.want_h264, demand.want_h265, demand.want_vp8,
                                     demand.want_vp9);

  if (demand.subscriber_count == 0) {
    if (screen_service_.IsCapturing()) {
      screen_service_.StopCapture();
      audit_logger_.AppendSystem("info", "screen.capture", "native capture stopped (no subscribers)");
    }
    active_capture_fps_ = 0;
    active_capture_scale_percent_ = kNativeScaleDefaultPercent;
    active_capture_video_bitrate_bps_ = kNativeBitrateDefaultBps;
    return;
  }

  const int target_fps = std::clamp(demand.fps > 0 ? demand.fps : kNativeCaptureFps, kNativeCaptureMinFps,
                                    kNativeCaptureMaxFps);
  const int target_scale_percent =
      std::clamp(demand.scale_percent > 0 ? demand.scale_percent : kNativeScaleDefaultPercent,
                 kNativeScaleMinPercent, kNativeScaleMaxPercent);
  const int target_video_bitrate_bps =
      std::clamp(demand.video_bitrate_bps > 0 ? demand.video_bitrate_bps : kNativeBitrateDefaultBps,
                 kNativeBitrateMinBps, kNativeBitrateMaxBps);

  screen_service_.SetEncodingProfile(target_scale_percent, target_video_bitrate_bps);
  active_capture_scale_percent_ = target_scale_percent;
  active_capture_video_bitrate_bps_ = target_video_bitrate_bps;

  if (screen_service_.IsCapturing() && active_capture_fps_ == target_fps) {
    return;
  }

  if (screen_service_.IsCapturing()) {
    screen_service_.StopCapture();
    active_capture_fps_ = 0;
  }

  std::string screen_error;
  if (!screen_service_.StartCapture(target_fps, &screen_error)) {
    std::cerr << "[ferryman] native screen capture unavailable: " << screen_error << '\n';
    audit_logger_.AppendSystem("warn", "screen.capture", screen_error);
    return;
  }
  active_capture_fps_ = target_fps;

  audit_logger_.AppendSystem("info", "screen.capture",
                             "native capture started (subscribers=" +
                                 std::to_string(demand.subscriber_count) +
                                 ", fps=" + std::to_string(target_fps) +
                                 ", scale=" + std::to_string(target_scale_percent) +
                                 ", bitrate=" + std::to_string(target_video_bitrate_bps) + ")");
}

void ServerApp::HandleWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
  const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(channel.get());
  std::string token;
  auto token_it = req->query_params.find("token");
  if (token_it != req->query_params.end()) {
    token = token_it->second;
  }

  auto session = session_manager_.GetSession(token);
  if (!session.has_value()) {
    audit_logger_.AppendSystem("warn", "ws.open.reject", "unauthorized token on " + req->path);
    channel->send(api::Error("unauthorized", "unauthorized"));
    channel->close();
    return;
  }

  std::string ws_path = req->path;
  const size_t qpos = ws_path.find('?');
  if (qpos != std::string::npos) {
    ws_path = ws_path.substr(0, qpos);
  }

  std::string channel_type;
  if (ws_path == "/ws/terminal") {
    channel_type = "terminal";
  } else if (ws_path == "/ws/webrtc") {
    channel_type = "webrtc";
  } else if (ws_path == "/ws/logs") {
    channel_type = "logs";
  } else {
    audit_logger_.Append(session->token, "ws.open.reject", "unknown path: " + req->path);
    channel->send(api::Error("unknown websocket path"));
    channel->close();
    return;
  }

  WsClient client;
  client.channel_type = channel_type;
  client.session_token = token;
  client.channel = channel;

  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    ws_clients_[key] = client;
  }

  audit_logger_.Append(token, "ws.open", channel_type);
  channel->send(api::Success({
      {"event", "connected", false},
      {"channel", channel_type, false},
  }));

  if (channel_type == "logs") {
    const std::string snapshot = api::Success({
        {"event", "logs_snapshot", false},
        {"items", audit_logger_.Tail(300), true},
    });
    channel->send(snapshot);
  }
}

void ServerApp::HandleWsMessage(const WebSocketChannelPtr& channel, const std::string& message) {
  const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(channel.get());
  std::string channel_type;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    auto it = ws_clients_.find(key);
    if (it == ws_clients_.end()) {
      return;
    }
    channel_type = it->second.channel_type;
  }

  if (channel_type == "terminal") {
    HandleTerminalWsMessage(key, message);
  } else if (channel_type == "webrtc") {
    HandleWebRtcWsMessage(key, message);
  } else if (channel_type == "logs") {
    HandleLogsWsMessage(key, message);
  } else {
    audit_logger_.AppendSystem("warn", "ws.message.reject", "unknown channel type");
  }
}

void ServerApp::HandleWsClose(const WebSocketChannelPtr& channel) {
  const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(channel.get());
  WsClient client;
  bool found = false;
  bool should_refresh_capture = false;

  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    auto it = ws_clients_.find(key);
    if (it != ws_clients_.end()) {
      client = it->second;
      should_refresh_capture = client.channel_type == "webrtc" && client.native_stream_subscribed;
      ws_clients_.erase(it);
      found = true;
    }
  }

  if (!found) {
    return;
  }

  if (client.channel_type == "terminal" && !client.terminal_id.empty()) {
    std::string error;
    pty_manager_.CloseTerminal(client.session_token, client.terminal_id, &error);
  }

  if (client.channel_type == "webrtc") {
    signaling_service_.Leave(key);
  }

  if (should_refresh_capture) {
    RefreshNativeCaptureState();
  }

  audit_logger_.Append(client.session_token, "ws.close", client.channel_type);
}

void ServerApp::HandleTerminalWsMessage(std::uintptr_t channel_key, const std::string& message) {
  const auto payload = ParseJsonOrNull(message);
  if (!payload.has_value() || !payload->is_object()) {
    audit_logger_.AppendSystem("warn", "terminal.ws.invalid", "invalid json payload");
    SendToWs(channel_key, api::Error("invalid json payload"));
    return;
  }
  const std::string action = JsonString(payload, "action");

  WsClient client;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    auto it = ws_clients_.find(channel_key);
    if (it == ws_clients_.end()) {
      return;
    }
    client = it->second;
  }

  auto session = session_manager_.GetSession(client.session_token);
  if (!session.has_value()) {
    audit_logger_.Append(client.session_token, "terminal.ws.reject", "session expired");
    SendToWs(channel_key, api::Error("session expired", "unauthorized"));
    return;
  }

  if (action == "open") {
    audit_logger_.Append(client.session_token, "terminal.open.request",
                         "cols=" + std::to_string(JsonInt(payload, "cols", 120)) +
                             ", rows=" + std::to_string(JsonInt(payload, "rows", 30)));

    std::string error;
    auto terminal_id = pty_manager_.CreateTerminal(client.session_token,
                                                   JsonInt(payload, "cols", 120),
                                                   JsonInt(payload, "rows", 30),
                                                   &error);
    if (!terminal_id.has_value()) {
      audit_logger_.Append(client.session_token, "terminal.open.error",
                           error.empty() ? "failed to create terminal" : error);
      SendToWs(channel_key, api::Error(error.empty() ? "failed to create terminal" : error));
      return;
    }

    {
      std::lock_guard<std::mutex> lock(ws_mu_);
      auto it = ws_clients_.find(channel_key);
      if (it != ws_clients_.end()) {
        it->second.terminal_id = *terminal_id;
      }
    }

    audit_logger_.Append(client.session_token, "terminal.open", *terminal_id);
    SendToWs(channel_key, api::Success({
        {"event", "terminal_open", false},
        {"terminal_id", *terminal_id, false},
    }));
    return;
  }

  if (action == "attach") {
    const std::string terminal_id = JsonString(payload, "terminal_id");
    const auto terminals = pty_manager_.ListTerminals(client.session_token);
    if (std::find(terminals.begin(), terminals.end(), terminal_id) == terminals.end()) {
      audit_logger_.Append(client.session_token, "terminal.attach.error", "terminal not found: " + terminal_id);
      SendToWs(channel_key, api::Error("terminal not found", "not_found"));
      return;
    }

    {
      std::lock_guard<std::mutex> lock(ws_mu_);
      auto it = ws_clients_.find(channel_key);
      if (it != ws_clients_.end()) {
        it->second.terminal_id = terminal_id;
      }
    }

    SendToWs(channel_key, api::Success({
        {"event", "terminal_attached", false},
        {"terminal_id", terminal_id, false},
    }));
    return;
  }

  std::string terminal_id = client.terminal_id;
  if (payload->contains("terminal_id")) {
    terminal_id = JsonString(payload, "terminal_id");
  }
  if (terminal_id.empty()) {
    audit_logger_.Append(client.session_token, "terminal.ws.reject", "terminal is not attached");
    SendToWs(channel_key, api::Error("terminal is not attached"));
    return;
  }

  if (action == "input") {
    const std::string encoded = JsonString(payload, "data");
    const std::string data = util::Base64Decode(encoded);
    std::string error;
    if (!pty_manager_.WriteInput(client.session_token, terminal_id, data, &error)) {
      audit_logger_.Append(client.session_token, "terminal.input.error",
                           error.empty() ? "write failed" : error);
      SendToWs(channel_key, api::Error(error.empty() ? "write failed" : error));
      return;
    }
    return;
  }

  if (action == "resize") {
    std::string error;
    if (!pty_manager_.Resize(client.session_token, terminal_id,
                             JsonInt(payload, "cols", 120),
                             JsonInt(payload, "rows", 30),
                             &error)) {
      audit_logger_.Append(client.session_token, "terminal.resize.error",
                           error.empty() ? "resize failed" : error);
      SendToWs(channel_key, api::Error(error.empty() ? "resize failed" : error));
      return;
    }
    return;
  }

  if (action == "close") {
    std::string error;
    if (!pty_manager_.CloseTerminal(client.session_token, terminal_id, &error)) {
      audit_logger_.Append(client.session_token, "terminal.close.error",
                           error.empty() ? "close failed" : error);
      SendToWs(channel_key, api::Error(error.empty() ? "close failed" : error));
      return;
    }
    {
      std::lock_guard<std::mutex> lock(ws_mu_);
      auto it = ws_clients_.find(channel_key);
      if (it != ws_clients_.end()) {
        it->second.terminal_id.clear();
      }
    }
    SendToWs(channel_key, api::Success({
        {"event", "terminal_closed", false},
        {"terminal_id", terminal_id, false},
    }));
    return;
  }

  audit_logger_.Append(client.session_token, "terminal.ws.invalid_action", action);
  SendToWs(channel_key, api::Error("unknown terminal action"));
}

void ServerApp::BroadcastTerminalOutput(const std::string& terminal_id, const std::string& chunk) {
  std::vector<WebSocketChannelPtr> channels;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    for (const auto& [_, client] : ws_clients_) {
      if (client.channel_type == "terminal" && client.terminal_id == terminal_id && client.channel) {
        channels.push_back(client.channel);
      }
    }
  }

  if (channels.empty()) {
    return;
  }

  const std::string payload = api::Success({
      {"event", "terminal_output", false},
      {"terminal_id", terminal_id, false},
      {"data", util::Base64Encode(chunk), false},
  });

  for (auto& channel : channels) {
    channel->send(payload);
  }
}

void ServerApp::BroadcastLogEntry(const std::string& serialized_entry) {
  std::vector<WebSocketChannelPtr> channels;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    for (const auto& [_, client] : ws_clients_) {
      if (client.channel_type == "logs" && client.channel) {
        channels.push_back(client.channel);
      }
    }
  }

  if (channels.empty()) {
    return;
  }

  const std::string payload = api::Success({
      {"event", "log_entry", false},
      {"item", serialized_entry, true},
  });
  for (auto& channel : channels) {
    channel->send(payload);
  }
}

void ServerApp::BroadcastNativeFrames() {
  uint64_t last_sequence = 0;
  while (running_) {
    auto frame = screen_service_.LatestFrame();
    if (!frame.has_value() || frame->sequence == last_sequence) {
      std::this_thread::sleep_for(std::chrono::milliseconds(24));
      continue;
    }
    last_sequence = frame->sequence;

    struct Target {
      WebSocketChannelPtr channel;
      std::string codec;
    };
    std::vector<Target> targets;
    {
      std::lock_guard<std::mutex> lock(ws_mu_);
      for (const auto& [_, client] : ws_clients_) {
        if (client.channel_type == "webrtc" && client.native_stream_subscribed && client.channel) {
          targets.push_back({client.channel, client.native_stream_codec});
        }
      }
    }
    if (targets.empty()) {
      continue;
    }

    const bool has_h264 = !frame->h264_bytes.empty();
    const bool has_h265 = !frame->h265_bytes.empty();
    const bool has_vp8 = !frame->vp8_bytes.empty();
    const bool has_vp9 = !frame->vp9_bytes.empty();
    const bool has_jpeg = !frame->jpeg_bytes.empty();
    if (!has_h264 && !has_h265 && !has_vp8 && !has_vp9 && !has_jpeg) {
      continue;
    }

    const std::string h264_payload = has_h264
        ? BuildNativeBinaryFramePacket(kNativeBinaryCodecH264, frame->h264_keyframe, frame->sequence,
                                       frame->captured_at_ms, frame->width, frame->height, frame->h264_bytes)
        : "";
    const std::string h265_payload = has_h265
        ? BuildNativeBinaryFramePacket(kNativeBinaryCodecH265, frame->h265_keyframe, frame->sequence,
                                       frame->captured_at_ms, frame->width, frame->height, frame->h265_bytes)
        : "";
    const std::string vp8_payload = has_vp8
        ? BuildNativeBinaryFramePacket(kNativeBinaryCodecVP8, frame->vp8_keyframe, frame->sequence,
                                       frame->captured_at_ms, frame->width, frame->height, frame->vp8_bytes)
        : "";
    const std::string vp9_payload = has_vp9
        ? BuildNativeBinaryFramePacket(kNativeBinaryCodecVP9, frame->vp9_keyframe, frame->sequence,
                                       frame->captured_at_ms, frame->width, frame->height, frame->vp9_bytes)
        : "";
    const std::string jpeg_payload = has_jpeg
        ? BuildNativeBinaryFramePacket(kNativeBinaryCodecJpeg, false, frame->sequence,
                                       frame->captured_at_ms, frame->width, frame->height, frame->jpeg_bytes)
        : "";

    for (auto& target : targets) {
      if (target.codec == "vp9" && !vp9_payload.empty()) {
        target.channel->send(vp9_payload, WS_OPCODE_BINARY);
        continue;
      }
      if (target.codec == "vp8" && !vp8_payload.empty()) {
        target.channel->send(vp8_payload, WS_OPCODE_BINARY);
        continue;
      }
      if (target.codec == "h265" && !h265_payload.empty()) {
        target.channel->send(h265_payload, WS_OPCODE_BINARY);
        continue;
      }
      if (target.codec == "h264" && !h264_payload.empty()) {
        target.channel->send(h264_payload, WS_OPCODE_BINARY);
        continue;
      }
      if (!jpeg_payload.empty()) {
        target.channel->send(jpeg_payload, WS_OPCODE_BINARY);
      }
    }
  }
}

void ServerApp::HandleWebRtcWsMessage(std::uintptr_t channel_key, const std::string& message) {
  const auto payload = ParseJsonOrNull(message);
  if (!payload.has_value() || !payload->is_object()) {
    audit_logger_.AppendSystem("warn", "webrtc.ws.invalid", "invalid json payload");
    SendToWs(channel_key, api::Error("invalid json payload"));
    return;
  }
  const std::string action = JsonString(payload, "action");

  WsClient client;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    auto it = ws_clients_.find(channel_key);
    if (it == ws_clients_.end()) {
      return;
    }
    client = it->second;
  }

  auto session = session_manager_.GetSession(client.session_token);
  if (!session.has_value()) {
    audit_logger_.Append(client.session_token, "webrtc.ws.reject", "session expired");
    SendToWs(channel_key, api::Error("session expired", "unauthorized"));
    return;
  }
  (void)session;

  if (action == "join") {
    const std::string room_id = JsonString(payload, "room_id");
    auto peer = signaling_service_.JoinRoom(channel_key, client.session_token, room_id);
    if (!peer.has_value()) {
      SendToWs(channel_key, api::Error("room_id is required"));
      return;
    }

    {
      std::lock_guard<std::mutex> lock(ws_mu_);
      auto it = ws_clients_.find(channel_key);
      if (it != ws_clients_.end()) {
        it->second.room_id = peer->room_id;
        it->second.peer_id = peer->peer_id;
      }
    }

    const auto peers = signaling_service_.PeersInRoom(peer->room_id);
    std::vector<std::string> peer_ids;
    std::vector<std::uintptr_t> notify_channels;
    for (const auto& item : peers) {
      if (item.peer_id != peer->peer_id) {
        peer_ids.push_back('"' + util::JsonEscape(item.peer_id) + '"');
        auto target = signaling_service_.FindChannelByPeerId(item.peer_id);
        if (target.has_value()) {
          notify_channels.push_back(*target);
        }
      }
    }

    SendToWs(channel_key, api::Success({
        {"event", "joined", false},
        {"room_id", peer->room_id, false},
        {"peer_id", peer->peer_id, false},
        {"peers", JsonArray(peer_ids), true},
    }));

    const std::string join_notice = api::Success({
        {"event", "peer_join", false},
        {"peer_id", peer->peer_id, false},
    });
    for (const auto& target_channel : notify_channels) {
      SendToWs(target_channel, join_notice);
    }

    audit_logger_.Append(client.session_token, "webrtc.join", room_id);
    return;
  }

  if (action == "signal") {
    const auto sender = signaling_service_.GetPeer(channel_key);
    if (!sender.has_value()) {
      SendToWs(channel_key, api::Error("join room first"));
      return;
    }

    const std::string signal_type = JsonString(payload, "signal_type");
    std::string signal_payload = JsonString(payload, "payload");
    if (payload->contains("payload") && !(*payload)["payload"].is_string()) {
      signal_payload = (*payload)["payload"].dump();
    }
    const std::string target_peer_id = JsonString(payload, "target_peer_id");

    std::vector<std::uintptr_t> targets;
    if (!target_peer_id.empty()) {
      auto target = signaling_service_.FindChannelByPeerId(target_peer_id);
      if (target.has_value()) {
        targets.push_back(*target);
      }
    } else {
      for (const auto& peer : signaling_service_.PeersInRoom(sender->room_id)) {
        if (peer.peer_id == sender->peer_id) {
          continue;
        }
        auto target = signaling_service_.FindChannelByPeerId(peer.peer_id);
        if (target.has_value()) {
          targets.push_back(*target);
        }
      }
    }

    const std::string forwarded = api::Success({
        {"event", "signal", false},
        {"from_peer_id", sender->peer_id, false},
        {"signal_type", signal_type, false},
        {"payload", signal_payload, false},
    });
    for (const auto& target : targets) {
      SendToWs(target, forwarded);
    }
    return;
  }

  if (action == "native_subscribe") {
    const std::string requested_codec = JsonString(payload, "codec");
    const bool wants_vp9 = requested_codec == "vp9";
    const bool wants_vp8 = requested_codec == "vp8";
    const bool wants_h265 = requested_codec == "h265";
    const bool wants_h264 = requested_codec == "h264";
    std::string negotiated_codec = "jpeg";
    if (wants_vp9 && screen_service_.SupportsVP9()) {
      negotiated_codec = "vp9";
    } else if (wants_vp8 && screen_service_.SupportsVP8()) {
      negotiated_codec = "vp8";
    } else if (wants_h265 && screen_service_.SupportsH265()) {
      negotiated_codec = "h265";
    } else if (wants_h264 && screen_service_.SupportsH264()) {
      negotiated_codec = "h264";
    }
    const int requested_fps =
        std::clamp(JsonInt(payload, "fps", kNativeCaptureFps), kNativeCaptureMinFps, kNativeCaptureMaxFps);
    const int requested_scale_percent = ParseNativeScalePercent(
        JsonString(payload, "resolution_tier"),
        std::clamp(JsonInt(payload, "scale_percent", kNativeScaleDefaultPercent),
                   kNativeScaleMinPercent, kNativeScaleMaxPercent));
    const int requested_video_bitrate_bps = ParseNativeBitrateBps(
        JsonString(payload, "bitrate_tier"),
        std::clamp(JsonInt(payload, "bitrate_bps", kNativeBitrateDefaultBps),
                   kNativeBitrateMinBps, kNativeBitrateMaxBps));

    {
      std::lock_guard<std::mutex> lock(ws_mu_);
      auto it = ws_clients_.find(channel_key);
      if (it != ws_clients_.end()) {
        it->second.native_stream_subscribed = true;
        it->second.native_stream_codec = negotiated_codec;
        it->second.native_stream_fps = requested_fps;
        it->second.native_stream_scale_percent = requested_scale_percent;
        it->second.native_stream_video_bitrate_bps = requested_video_bitrate_bps;
      }
    }

    RefreshNativeCaptureState();
    const int effective_fps = active_capture_fps_.load();
    const int effective_scale_percent = active_capture_scale_percent_.load();
    const int effective_video_bitrate_bps = active_capture_video_bitrate_bps_.load();

    SendToWs(channel_key, api::Success({
        {"event", "native_subscribed", false},
        {"capture_running", screen_service_.IsCapturing() ? "true" : "false", true},
        {"codec", negotiated_codec, false},
        {"fps", std::to_string(effective_fps > 0 ? effective_fps : requested_fps), true},
        {"resolution_tier", NativeResolutionTierFromScale(effective_scale_percent), false},
        {"scale_percent", std::to_string(effective_scale_percent), true},
        {"bitrate_tier", NativeBitrateTierFromBps(effective_video_bitrate_bps), false},
        {"bitrate_bps", std::to_string(effective_video_bitrate_bps), true},
        {"transport", "ws-binary", false},
    }));
    return;
  }

  if (action == "native_unsubscribe") {
    {
      std::lock_guard<std::mutex> lock(ws_mu_);
      auto it = ws_clients_.find(channel_key);
      if (it != ws_clients_.end()) {
        it->second.native_stream_subscribed = false;
      }
    }

    RefreshNativeCaptureState();

    SendToWs(channel_key, api::Success({
        {"event", "native_unsubscribed", false},
    }));
    return;
  }

  if (action == "input_event") {
    const std::string type = JsonString(payload, "type");
    std::string event_payload = JsonString(payload, "payload");
    if (payload->contains("payload") && !(*payload)["payload"].is_string()) {
      event_payload = (*payload)["payload"].dump();
    }

    std::string error;
    if (!screen_service_.InjectInputEvent(client.session_token, {type, event_payload}, &error)) {
      SendToWs(channel_key, api::Error(error, "forbidden"));
      return;
    }

    audit_logger_.Append(client.session_token, "screen.input.ws", type);
    SendToWs(channel_key, api::Success({
        {"event", "input_ack", false},
        {"message", error, false},
    }));
    return;
  }

  SendToWs(channel_key, api::Error("unknown webrtc action"));
  audit_logger_.Append(client.session_token, "webrtc.ws.invalid_action", action);
}

void ServerApp::HandleLogsWsMessage(std::uintptr_t channel_key, const std::string& message) {
  const auto payload = ParseJsonOrNull(message);
  if (!payload.has_value() || !payload->is_object()) {
    SendToWs(channel_key, api::Error("invalid json payload"));
    return;
  }

  WsClient client;
  {
    std::lock_guard<std::mutex> lock(ws_mu_);
    auto it = ws_clients_.find(channel_key);
    if (it == ws_clients_.end()) {
      return;
    }
    client = it->second;
  }

  auto session = session_manager_.GetSession(client.session_token);
  if (!session.has_value()) {
    SendToWs(channel_key, api::Error("session expired", "unauthorized"));
    return;
  }
  (void)session;

  const std::string action = JsonString(payload, "action", "tail");
  if (action == "tail" || action == "snapshot") {
    const int lines = std::clamp(JsonInt(payload, "lines", 300), 1, 2000);
    SendToWs(channel_key, api::Success({
        {"event", "logs_snapshot", false},
        {"items", audit_logger_.Tail(static_cast<size_t>(lines)), true},
    }));
    return;
  }

  SendToWs(channel_key, api::Error("unknown logs action"));
}

#endif

}  // namespace ferryman::web
