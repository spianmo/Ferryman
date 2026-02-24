#include "ferryman/web/ScreenService.hpp"

#include "ferryman/util/StringUtil.hpp"

#if defined(__APPLE__)
#include "ferryman/web/ScreenCaptureKitBridge.hpp"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#endif

#if defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#if !defined(FERRYMAN_WITH_FFMPEG)
#define FERRYMAN_WITH_FFMPEG 0
#endif

#if FERRYMAN_WITH_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}
#endif

namespace ferryman::web {

namespace {

using nlohmann::json;

int64_t EpochMillisNow() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string JsonCapabilities(bool native_stream, bool input_injection, const std::string& backend,
                             bool supports_h264, bool supports_h265, bool supports_vp8,
                             bool supports_vp9, bool supports_av1) {
  json native_codecs = json::array();
  if (native_stream) {
    native_codecs.push_back("jpeg");
    if (supports_h264) {
      native_codecs.push_back("h264");
    }
    if (supports_h265) {
      native_codecs.push_back("h265");
    }
    if (supports_vp8) {
      native_codecs.push_back("vp8");
    }
    if (supports_vp9) {
      native_codecs.push_back("vp9");
    }
    if (supports_av1) {
      native_codecs.push_back("av1");
    }
  }
  std::vector<std::string> video_codecs;
  if (supports_h264) {
    video_codecs.emplace_back("h264");
  }
  if (supports_h265) {
    video_codecs.emplace_back("h265");
  }
  if (supports_vp8) {
    video_codecs.emplace_back("vp8");
  }
  if (supports_vp9) {
    video_codecs.emplace_back("vp9");
  }
  if (supports_av1) {
    video_codecs.emplace_back("av1");
  }

  std::string encoding = "unsupported";
  if (!video_codecs.empty()) {
    encoding = "ffmpeg-";
    for (size_t idx = 0; idx < video_codecs.size(); ++idx) {
      if (idx > 0) {
        encoding.push_back('+');
      }
      encoding += video_codecs[idx];
    }
  }
  json payload = {
      {"webrtc_signaling", true},
      {"native_screen_stream", native_stream},
      {"input_injection", input_injection},
      {"transport", native_stream ? "webrtc-signaling+native-ws" : "p2p-signaling"},
      {"screen_backend", backend},
      {"video_encoding", encoding},
      {"native_stream_codecs", native_codecs},
      {"native_stream_transport", native_stream ? "ws-binary" : "unsupported"},
      {"native_resolution_tiers", json::array({
                                    json{
                                        {"id", "full"},
                                        {"scale_percent", 100},
                                    },
                                    json{
                                        {"id", "balanced"},
                                        {"scale_percent", 75},
                                    },
                                    json{
                                        {"id", "performance"},
                                        {"scale_percent", 50},
                                    },
                                })},
      {"native_bitrate_tiers", json::array({
                                  json{
                                      {"id", "sd"},
                                      {"bitrate_bps", 1'500'000},
                                  },
                                  json{
                                      {"id", "hd"},
                                      {"bitrate_bps", 3'000'000},
                                  },
                                  json{
                                      {"id", "uhd"},
                                      {"bitrate_bps", 6'000'000},
                                  },
                              })},
  };
  return payload.dump();
}

bool JsonBoolWithAliases(const json& payload, std::initializer_list<const char*> keys,
                         bool fallback = false) {
  for (const char* key : keys) {
    const auto it = payload.find(key);
    if (it == payload.end()) {
      continue;
    }
    if (it->is_boolean()) {
      return it->get<bool>();
    }
    if (it->is_number_integer()) {
      return it->get<int>() != 0;
    }
    if (it->is_string()) {
      std::string lower = it->get<std::string>();
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
    }
  }
  return fallback;
}

std::string PayloadCode(const json& payload) {
  const auto it = payload.find("code");
  if (it != payload.end() && it->is_string()) {
    return it->get<std::string>();
  }
  const auto dom_it = payload.find("dom_code");
  if (dom_it != payload.end() && dom_it->is_string()) {
    return dom_it->get<std::string>();
  }
  return "";
}

std::string PayloadKey(const json& payload) {
  const auto it = payload.find("key");
  if (it != payload.end() && it->is_string()) {
    return it->get<std::string>();
  }
  return "";
}

#if FERRYMAN_WITH_FFMPEG
std::string AvErrorToString(int code) {
  char error[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(code, error, sizeof(error));
  return std::string(error);
}

bool EncodeFrameToJpegFfmpeg(const uint8_t* source, int width, int height, int stride,
                             AVPixelFormat source_pix_fmt, std::string* jpeg_bytes,
                             std::string* error) {
  if (source == nullptr || width <= 0 || height <= 0 || stride <= 0 ||
      source_pix_fmt == AV_PIX_FMT_NONE || jpeg_bytes == nullptr) {
    if (error != nullptr) {
      *error = "invalid frame buffer for ffmpeg jpeg encoding";
    }
    return false;
  }

  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  if (codec == nullptr) {
    if (error != nullptr) {
      *error = "ffmpeg encoder AV_CODEC_ID_MJPEG is unavailable";
    }
    return false;
  }

  AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
  if (codec_ctx == nullptr) {
    if (error != nullptr) {
      *error = "failed to allocate ffmpeg mjpeg codec context";
    }
    return false;
  }

  codec_ctx->width = width;
  codec_ctx->height = height;
  codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  codec_ctx->color_range = AVCOL_RANGE_JPEG;
  codec_ctx->time_base = AVRational{1, 25};
  codec_ctx->framerate = AVRational{25, 1};

  int ret = avcodec_open2(codec_ctx, codec, nullptr);
  if (ret < 0) {
    if (error != nullptr) {
      *error = "failed to open ffmpeg mjpeg encoder: " + AvErrorToString(ret);
    }
    avcodec_free_context(&codec_ctx);
    return false;
  }

  AVFrame* frame = av_frame_alloc();
  if (frame == nullptr) {
    if (error != nullptr) {
      *error = "failed to allocate ffmpeg mjpeg frame";
    }
    avcodec_free_context(&codec_ctx);
    return false;
  }

  frame->format = codec_ctx->pix_fmt;
  frame->width = width;
  frame->height = height;
  frame->color_range = codec_ctx->color_range;

  ret = av_frame_get_buffer(frame, 32);
  if (ret < 0) {
    if (error != nullptr) {
      *error = "failed to allocate ffmpeg mjpeg frame buffer: " + AvErrorToString(ret);
    }
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    return false;
  }

  SwsContext* sws_ctx = sws_getContext(width, height, source_pix_fmt, width, height, codec_ctx->pix_fmt,
                                       SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (sws_ctx == nullptr) {
    if (error != nullptr) {
      *error = "failed to create ffmpeg sws context for jpeg encoding";
    }
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    return false;
  }

  const uint8_t* src_data[1] = {source};
  int src_linesize[1] = {stride};
  sws_scale(sws_ctx, src_data, src_linesize, 0, height, frame->data, frame->linesize);
  frame->pts = 0;

  AVPacket* packet = av_packet_alloc();
  if (packet == nullptr) {
    if (error != nullptr) {
      *error = "failed to allocate ffmpeg mjpeg packet";
    }
    sws_freeContext(sws_ctx);
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    return false;
  }

  ret = avcodec_send_frame(codec_ctx, frame);
  if (ret < 0) {
    if (error != nullptr) {
      *error = "failed to send frame to ffmpeg mjpeg encoder: " + AvErrorToString(ret);
    }
    av_packet_free(&packet);
    sws_freeContext(sws_ctx);
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    return false;
  }

  jpeg_bytes->clear();
  while (true) {
    ret = avcodec_receive_packet(codec_ctx, packet);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      break;
    }
    if (ret < 0) {
      if (error != nullptr) {
        *error = "failed to receive ffmpeg mjpeg packet: " + AvErrorToString(ret);
      }
      av_packet_free(&packet);
      sws_freeContext(sws_ctx);
      av_frame_free(&frame);
      avcodec_free_context(&codec_ctx);
      return false;
    }
    if (packet->size > 0 && packet->data != nullptr) {
      jpeg_bytes->append(reinterpret_cast<const char*>(packet->data),
                         static_cast<size_t>(packet->size));
    }
    av_packet_unref(packet);
  }

  av_packet_free(&packet);
  sws_freeContext(sws_ctx);
  av_frame_free(&frame);
  avcodec_free_context(&codec_ctx);

  if (jpeg_bytes->empty()) {
    if (error != nullptr) {
      *error = "ffmpeg mjpeg encoder produced empty packet";
    }
    return false;
  }
  return true;
}

int BytesPerPixelForPackedFormat(AVPixelFormat format) {
  switch (format) {
    case AV_PIX_FMT_BGRA:
      return 4;
    case AV_PIX_FMT_BGR24:
      return 3;
    default:
      return 0;
  }
}

bool ScaleFrameWithFfmpeg(const uint8_t* source, int width, int height, int stride,
                          AVPixelFormat pix_fmt, int target_width, int target_height,
                          std::string* scaled_bytes, int* scaled_stride, std::string* error) {
  if (source == nullptr || width <= 0 || height <= 0 || stride <= 0 || target_width <= 0 ||
      target_height <= 0 || scaled_bytes == nullptr || scaled_stride == nullptr) {
    if (error != nullptr) {
      *error = "invalid frame buffer for ffmpeg scaling";
    }
    return false;
  }

  const int bytes_per_pixel = BytesPerPixelForPackedFormat(pix_fmt);
  if (bytes_per_pixel <= 0) {
    if (error != nullptr) {
      *error = "unsupported pixel format for scaling";
    }
    return false;
  }

  *scaled_stride = target_width * bytes_per_pixel;
  scaled_bytes->assign(static_cast<size_t>(*scaled_stride) * static_cast<size_t>(target_height), '\0');

  SwsContext* sws_ctx = sws_getContext(width, height, pix_fmt, target_width, target_height, pix_fmt,
                                       SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
  if (sws_ctx == nullptr) {
    if (error != nullptr) {
      *error = "failed to create ffmpeg sws context for scaling";
    }
    return false;
  }

  const uint8_t* src_data[4] = {source, nullptr, nullptr, nullptr};
  int src_linesize[4] = {stride, 0, 0, 0};
  uint8_t* dst_data[4] = {reinterpret_cast<uint8_t*>(scaled_bytes->data()), nullptr, nullptr, nullptr};
  int dst_linesize[4] = {*scaled_stride, 0, 0, 0};
  sws_scale(sws_ctx, src_data, src_linesize, 0, height, dst_data, dst_linesize);
  sws_freeContext(sws_ctx);
  return true;
}
#endif

#if defined(__APPLE__)
CGPoint NormalizePointToMainDisplay(const json& payload, double fallback_x, double fallback_y,
                                    double* out_x, double* out_y) {
  const CGRect bounds = CGDisplayBounds(CGMainDisplayID());

  const double input_x = payload.value("x", fallback_x);
  const double input_y = payload.value("y", fallback_y);
  const double view_w = payload.value("width", bounds.size.width);
  const double view_h = payload.value("height", bounds.size.height);

  double mapped_x = input_x;
  double mapped_y = input_y;

  if (view_w > 1.0 && view_h > 1.0) {
    mapped_x = bounds.origin.x + std::clamp(input_x / view_w, 0.0, 1.0) * bounds.size.width;
    mapped_y = bounds.origin.y + std::clamp(input_y / view_h, 0.0, 1.0) * bounds.size.height;
  }

  if (out_x != nullptr) {
    *out_x = mapped_x;
  }
  if (out_y != nullptr) {
    *out_y = mapped_y;
  }
  return CGPointMake(mapped_x, mapped_y);
}

CGMouseButton ParseMouseButtonMac(int button) {
  if (button == 1) {
    return kCGMouseButtonRight;
  }
  if (button == 2) {
    return kCGMouseButtonCenter;
  }
  return kCGMouseButtonLeft;
}

std::pair<CGEventType, CGEventType> MouseDownUpTypeMac(CGMouseButton button) {
  if (button == kCGMouseButtonRight) {
    return {kCGEventRightMouseDown, kCGEventRightMouseUp};
  }
  if (button == kCGMouseButtonCenter) {
    return {kCGEventOtherMouseDown, kCGEventOtherMouseUp};
  }
  return {kCGEventLeftMouseDown, kCGEventLeftMouseUp};
}

const std::unordered_map<std::string, int>& BrowserCodeToMacKeycode() {
  static const auto* kMap = new std::unordered_map<std::string, int>{
      {"KeyA", 0},          {"KeyS", 1},          {"KeyD", 2},          {"KeyF", 3},
      {"KeyH", 4},          {"KeyG", 5},          {"KeyZ", 6},          {"KeyX", 7},
      {"KeyC", 8},          {"KeyV", 9},          {"KeyB", 11},         {"KeyQ", 12},
      {"KeyW", 13},         {"KeyE", 14},         {"KeyR", 15},         {"KeyY", 16},
      {"KeyT", 17},         {"Digit1", 18},       {"Digit2", 19},       {"Digit3", 20},
      {"Digit4", 21},       {"Digit6", 22},       {"Digit5", 23},       {"Equal", 24},
      {"Digit9", 25},       {"Digit7", 26},       {"Minus", 27},        {"Digit8", 28},
      {"Digit0", 29},       {"BracketRight", 30}, {"KeyO", 31},         {"KeyU", 32},
      {"BracketLeft", 33},  {"KeyI", 34},         {"KeyP", 35},         {"Enter", 36},
      {"KeyL", 37},         {"KeyJ", 38},         {"Quote", 39},        {"KeyK", 40},
      {"Semicolon", 41},    {"Backslash", 42},    {"Comma", 43},        {"Slash", 44},
      {"KeyN", 45},         {"KeyM", 46},         {"Period", 47},       {"Tab", 48},
      {"Space", 49},        {"Backquote", 50},    {"Backspace", 51},    {"Escape", 53},
      {"MetaRight", 54},    {"MetaLeft", 55},     {"ShiftLeft", 56},    {"CapsLock", 57},
      {"AltLeft", 58},      {"ControlLeft", 59},  {"ShiftRight", 60},   {"AltRight", 61},
      {"ControlRight", 62}, {"Fn", 63},           {"NumpadDecimal", 65},
      {"NumpadMultiply", 67},                         {"NumpadAdd", 69},
      {"NumLock", 71},                                {"NumpadDivide", 75},
      {"NumpadEnter", 76},                            {"NumpadSubtract", 78},
      {"NumpadEqual", 81},                            {"Numpad0", 82},
      {"Numpad1", 83},                                {"Numpad2", 84},
      {"Numpad3", 85},                                {"Numpad4", 86},
      {"Numpad5", 87},                                {"Numpad6", 88},
      {"Numpad7", 89},                                {"Numpad8", 91},
      {"Numpad9", 92},                                {"F5", 96},
      {"F6", 97},                                     {"F7", 98},
      {"F3", 99},                                     {"F8", 100},
      {"F9", 101},                                    {"F11", 103},
      {"F13", 105},                                   {"F16", 106},
      {"F14", 107},                                   {"F10", 109},
      {"F12", 111},                                   {"F15", 113},
      {"Help", 114},                                  {"Home", 115},
      {"PageUp", 116},                                {"Delete", 117},
      {"F4", 118},                                    {"End", 119},
      {"F2", 120},                                    {"PageDown", 121},
      {"F1", 122},                                    {"ArrowLeft", 123},
      {"ArrowRight", 124},                            {"ArrowDown", 125},
      {"ArrowUp", 126},
  };
  return *kMap;
}

const std::unordered_map<std::string, int>& BrowserKeyToMacKeycode() {
  static const auto* kMap = new std::unordered_map<std::string, int>{
      {"Enter", 36},      {"Tab", 48},          {" ", 49},           {"Spacebar", 49},
      {"Backspace", 51},  {"Escape", 53},       {"Esc", 53},         {"ArrowLeft", 123},
      {"ArrowRight", 124},{"ArrowDown", 125},   {"ArrowUp", 126},    {"Home", 115},
      {"End", 119},       {"PageUp", 116},      {"PageDown", 121},   {"Delete", 117},
      {"Insert", 114},    {"F1", 122},          {"F2", 120},         {"F3", 99},
      {"F4", 118},        {"F5", 96},           {"F6", 97},          {"F7", 98},
      {"F8", 100},        {"F9", 101},          {"F10", 109},        {"F11", 103},
      {"F12", 111},       {"F13", 105},         {"F14", 107},        {"F15", 113},
      {"F16", 106},       {"F17", 64},          {"F18", 79},         {"F19", 80},
      {"F20", 90},
  };
  return *kMap;
}

std::optional<int> ResolveMacKeyCode(const json& payload) {
  const auto numeric_it = payload.find("key_code");
  if (numeric_it != payload.end()) {
    if (numeric_it->is_number_integer()) {
      const int key_code = numeric_it->get<int>();
      if (key_code >= 0 && key_code <= 255) {
        return key_code;
      }
    }
    return std::nullopt;
  }

  const std::string code = PayloadCode(payload);
  if (!code.empty()) {
    const auto it = BrowserCodeToMacKeycode().find(code);
    if (it != BrowserCodeToMacKeycode().end()) {
      return it->second;
    }
  }

  const std::string key = PayloadKey(payload);
  if (!key.empty()) {
    const auto key_it = BrowserKeyToMacKeycode().find(key);
    if (key_it != BrowserKeyToMacKeycode().end()) {
      return key_it->second;
    }

    if (key.size() == 1) {
      const unsigned char ch = static_cast<unsigned char>(key[0]);
      const char upper = static_cast<char>(std::toupper(ch));
      if (upper >= 'A' && upper <= 'Z') {
        std::string dom_code = "Key";
        dom_code.push_back(upper);
        const auto letter = BrowserCodeToMacKeycode().find(dom_code);
        if (letter != BrowserCodeToMacKeycode().end()) {
          return letter->second;
        }
      }
      if (upper >= '0' && upper <= '9') {
        std::string dom_code = "Digit";
        dom_code.push_back(upper);
        const auto digit = BrowserCodeToMacKeycode().find(dom_code);
        if (digit != BrowserCodeToMacKeycode().end()) {
          return digit->second;
        }
      }
    }
  }

  return std::nullopt;
}

CGEventFlags ModifierFlagsFromPayloadMac(const json& payload) {
  CGEventFlags flags = 0;
  if (JsonBoolWithAliases(payload, {"shiftKey", "shift", "shift_key"})) {
    flags |= kCGEventFlagMaskShift;
  }
  if (JsonBoolWithAliases(payload, {"ctrlKey", "controlKey", "ctrl", "control", "ctrl_key"})) {
    flags |= kCGEventFlagMaskControl;
  }
  if (JsonBoolWithAliases(payload, {"altKey", "optionKey", "alt", "option", "alt_key"})) {
    flags |= kCGEventFlagMaskAlternate;
  }
  if (JsonBoolWithAliases(payload, {"metaKey", "commandKey", "meta", "command", "meta_key"})) {
    flags |= kCGEventFlagMaskCommand;
  }
  return flags;
}
#endif

#if defined(__linux__)
unsigned long MaskShift(unsigned long mask) {
  unsigned long shift = 0;
  while (mask != 0 && (mask & 1UL) == 0UL) {
    mask >>= 1;
    ++shift;
  }
  return shift;
}

unsigned long MaskBitCount(unsigned long mask) {
  unsigned long bits = 0;
  while (mask != 0) {
    bits += mask & 1UL;
    mask >>= 1;
  }
  return bits;
}

uint8_t NormalizeMaskValue(unsigned long pixel, unsigned long mask) {
  if (mask == 0) {
    return 0;
  }
  const unsigned long shift = MaskShift(mask);
  const unsigned long bits = MaskBitCount(mask);
  const unsigned long raw = (pixel & mask) >> shift;
  if (bits == 0) {
    return 0;
  }
  if (bits >= 8) {
    return static_cast<uint8_t>(raw >> (bits - 8));
  }
  const unsigned long max_value = (1UL << bits) - 1UL;
  if (max_value == 0) {
    return 0;
  }
  return static_cast<uint8_t>((raw * 255UL) / max_value);
}

int LinuxButtonFromBrowserButton(int browser_button) {
  if (browser_button == 1) {
    return 3;
  }
  if (browser_button == 2) {
    return 2;
  }
  return 1;
}

std::optional<KeySym> BrowserCodeToLinuxKeySym(const std::string& code) {
  if (code.size() == 4 && code.rfind("Key", 0) == 0) {
    const char letter = static_cast<char>(std::tolower(static_cast<unsigned char>(code[3])));
    return static_cast<KeySym>(letter);
  }
  if (code.size() == 6 && code.rfind("Digit", 0) == 0 && std::isdigit(static_cast<unsigned char>(code[5]))) {
    return static_cast<KeySym>(code[5]);
  }

  static const auto* kCodeMap = new std::unordered_map<std::string, KeySym>{
      {"Enter", XK_Return},            {"Tab", XK_Tab},               {"Space", XK_space},
      {"Backspace", XK_BackSpace},     {"Escape", XK_Escape},         {"ArrowLeft", XK_Left},
      {"ArrowRight", XK_Right},        {"ArrowUp", XK_Up},            {"ArrowDown", XK_Down},
      {"Home", XK_Home},               {"End", XK_End},               {"PageUp", XK_Page_Up},
      {"PageDown", XK_Page_Down},      {"Insert", XK_Insert},         {"Delete", XK_Delete},
      {"Minus", XK_minus},             {"Equal", XK_equal},           {"BracketLeft", XK_bracketleft},
      {"BracketRight", XK_bracketright},{"Backslash", XK_backslash},  {"Semicolon", XK_semicolon},
      {"Quote", XK_apostrophe},        {"Backquote", XK_grave},       {"Comma", XK_comma},
      {"Period", XK_period},           {"Slash", XK_slash},           {"CapsLock", XK_Caps_Lock},
      {"ShiftLeft", XK_Shift_L},       {"ShiftRight", XK_Shift_R},    {"ControlLeft", XK_Control_L},
      {"ControlRight", XK_Control_R},  {"AltLeft", XK_Alt_L},         {"AltRight", XK_Alt_R},
      {"MetaLeft", XK_Super_L},        {"MetaRight", XK_Super_R},     {"F1", XK_F1},
      {"F2", XK_F2},                   {"F3", XK_F3},                 {"F4", XK_F4},
      {"F5", XK_F5},                   {"F6", XK_F6},                 {"F7", XK_F7},
      {"F8", XK_F8},                   {"F9", XK_F9},                 {"F10", XK_F10},
      {"F11", XK_F11},                 {"F12", XK_F12},               {"Numpad0", XK_KP_0},
      {"Numpad1", XK_KP_1},            {"Numpad2", XK_KP_2},          {"Numpad3", XK_KP_3},
      {"Numpad4", XK_KP_4},            {"Numpad5", XK_KP_5},          {"Numpad6", XK_KP_6},
      {"Numpad7", XK_KP_7},            {"Numpad8", XK_KP_8},          {"Numpad9", XK_KP_9},
      {"NumpadAdd", XK_KP_Add},        {"NumpadSubtract", XK_KP_Subtract},
      {"NumpadMultiply", XK_KP_Multiply}, {"NumpadDivide", XK_KP_Divide},
      {"NumpadEnter", XK_KP_Enter},    {"NumpadDecimal", XK_KP_Decimal},
  };

  const auto it = kCodeMap->find(code);
  if (it != kCodeMap->end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<KeySym> BrowserKeyToLinuxKeySym(const std::string& key) {
  if (key.empty()) {
    return std::nullopt;
  }
  if (key.size() == 1) {
    const std::string one(1, key[0]);
    const KeySym symbol = XStringToKeysym(one.c_str());
    if (symbol != NoSymbol) {
      return symbol;
    }
  }

  static const auto* kKeyMap = new std::unordered_map<std::string, KeySym>{
      {" ", XK_space},          {"Spacebar", XK_space},   {"Enter", XK_Return},
      {"Tab", XK_Tab},          {"Backspace", XK_BackSpace},
      {"Escape", XK_Escape},    {"Esc", XK_Escape},       {"ArrowLeft", XK_Left},
      {"ArrowRight", XK_Right}, {"ArrowUp", XK_Up},       {"ArrowDown", XK_Down},
      {"Home", XK_Home},        {"End", XK_End},          {"PageUp", XK_Page_Up},
      {"PageDown", XK_Page_Down}, {"Insert", XK_Insert},  {"Delete", XK_Delete},
      {"Shift", XK_Shift_L},    {"Control", XK_Control_L},{"Alt", XK_Alt_L},
      {"Meta", XK_Super_L},     {"F1", XK_F1},            {"F2", XK_F2},
      {"F3", XK_F3},            {"F4", XK_F4},            {"F5", XK_F5},
      {"F6", XK_F6},            {"F7", XK_F7},            {"F8", XK_F8},
      {"F9", XK_F9},            {"F10", XK_F10},          {"F11", XK_F11},
      {"F12", XK_F12},
  };

  const auto it = kKeyMap->find(key);
  if (it != kKeyMap->end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<KeySym> ResolveLinuxKeySym(const json& payload) {
  const std::string code = PayloadCode(payload);
  if (!code.empty()) {
    const auto mapped = BrowserCodeToLinuxKeySym(code);
    if (mapped.has_value()) {
      return mapped;
    }
  }

  const std::string key = PayloadKey(payload);
  if (!key.empty()) {
    const auto mapped = BrowserKeyToLinuxKeySym(key);
    if (mapped.has_value()) {
      return mapped;
    }
  }

  return std::nullopt;
}

void EmitLinuxModifier(Display* display, KeySym symbol, bool down) {
  const KeyCode code = XKeysymToKeycode(display, symbol);
  if (code != 0) {
    XTestFakeKeyEvent(display, code, down ? True : False, CurrentTime);
  }
}

void EmitLinuxPayloadModifiers(Display* display, const json& payload, bool down) {
  if (JsonBoolWithAliases(payload, {"shiftKey", "shift", "shift_key"})) {
    EmitLinuxModifier(display, XK_Shift_L, down);
  }
  if (JsonBoolWithAliases(payload, {"ctrlKey", "controlKey", "ctrl", "control", "ctrl_key"})) {
    EmitLinuxModifier(display, XK_Control_L, down);
  }
  if (JsonBoolWithAliases(payload, {"altKey", "optionKey", "alt", "option", "alt_key"})) {
    EmitLinuxModifier(display, XK_Alt_L, down);
  }
  if (JsonBoolWithAliases(payload, {"metaKey", "commandKey", "meta", "command", "meta_key"})) {
    EmitLinuxModifier(display, XK_Super_L, down);
  }
}
#endif

#if defined(_WIN32)
int WindowsButtonDownFlag(int button) {
  if (button == 1) {
    return MOUSEEVENTF_RIGHTDOWN;
  }
  if (button == 2) {
    return MOUSEEVENTF_MIDDLEDOWN;
  }
  return MOUSEEVENTF_LEFTDOWN;
}

int WindowsButtonUpFlag(int button) {
  if (button == 1) {
    return MOUSEEVENTF_RIGHTUP;
  }
  if (button == 2) {
    return MOUSEEVENTF_MIDDLEUP;
  }
  return MOUSEEVENTF_LEFTUP;
}

std::optional<WORD> BrowserCodeToWindowsVk(const std::string& code) {
  if (code.size() == 4 && code.rfind("Key", 0) == 0) {
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(code[3])));
    if (upper >= 'A' && upper <= 'Z') {
      return static_cast<WORD>(upper);
    }
  }
  if (code.size() == 6 && code.rfind("Digit", 0) == 0 && std::isdigit(static_cast<unsigned char>(code[5]))) {
    return static_cast<WORD>(code[5]);
  }

  static const auto* kCodeMap = new std::unordered_map<std::string, WORD>{
      {"Enter", VK_RETURN},          {"Tab", VK_TAB},             {"Space", VK_SPACE},
      {"Backspace", VK_BACK},        {"Escape", VK_ESCAPE},       {"ArrowLeft", VK_LEFT},
      {"ArrowRight", VK_RIGHT},      {"ArrowUp", VK_UP},          {"ArrowDown", VK_DOWN},
      {"Home", VK_HOME},             {"End", VK_END},             {"PageUp", VK_PRIOR},
      {"PageDown", VK_NEXT},         {"Insert", VK_INSERT},       {"Delete", VK_DELETE},
      {"Minus", VK_OEM_MINUS},       {"Equal", VK_OEM_PLUS},      {"BracketLeft", VK_OEM_4},
      {"BracketRight", VK_OEM_6},    {"Backslash", VK_OEM_5},     {"Semicolon", VK_OEM_1},
      {"Quote", VK_OEM_7},           {"Backquote", VK_OEM_3},     {"Comma", VK_OEM_COMMA},
      {"Period", VK_OEM_PERIOD},     {"Slash", VK_OEM_2},         {"CapsLock", VK_CAPITAL},
      {"ShiftLeft", VK_LSHIFT},      {"ShiftRight", VK_RSHIFT},   {"ControlLeft", VK_LCONTROL},
      {"ControlRight", VK_RCONTROL}, {"AltLeft", VK_LMENU},       {"AltRight", VK_RMENU},
      {"MetaLeft", VK_LWIN},         {"MetaRight", VK_RWIN},      {"F1", VK_F1},
      {"F2", VK_F2},                 {"F3", VK_F3},               {"F4", VK_F4},
      {"F5", VK_F5},                 {"F6", VK_F6},               {"F7", VK_F7},
      {"F8", VK_F8},                 {"F9", VK_F9},               {"F10", VK_F10},
      {"F11", VK_F11},               {"F12", VK_F12},             {"Numpad0", VK_NUMPAD0},
      {"Numpad1", VK_NUMPAD1},       {"Numpad2", VK_NUMPAD2},     {"Numpad3", VK_NUMPAD3},
      {"Numpad4", VK_NUMPAD4},       {"Numpad5", VK_NUMPAD5},     {"Numpad6", VK_NUMPAD6},
      {"Numpad7", VK_NUMPAD7},       {"Numpad8", VK_NUMPAD8},     {"Numpad9", VK_NUMPAD9},
      {"NumpadAdd", VK_ADD},         {"NumpadSubtract", VK_SUBTRACT},
      {"NumpadMultiply", VK_MULTIPLY}, {"NumpadDivide", VK_DIVIDE},
      {"NumpadEnter", VK_RETURN},    {"NumpadDecimal", VK_DECIMAL},
  };

  const auto it = kCodeMap->find(code);
  if (it != kCodeMap->end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<WORD> BrowserKeyToWindowsVk(const std::string& key) {
  if (key.empty()) {
    return std::nullopt;
  }
  if (key.size() == 1) {
    const SHORT result = VkKeyScanA(key[0]);
    if (result != -1) {
      return static_cast<WORD>(result & 0xff);
    }
  }

  static const auto* kKeyMap = new std::unordered_map<std::string, WORD>{
      {" ", VK_SPACE},             {"Spacebar", VK_SPACE},        {"Enter", VK_RETURN},
      {"Tab", VK_TAB},             {"Backspace", VK_BACK},        {"Escape", VK_ESCAPE},
      {"Esc", VK_ESCAPE},          {"ArrowLeft", VK_LEFT},        {"ArrowRight", VK_RIGHT},
      {"ArrowUp", VK_UP},          {"ArrowDown", VK_DOWN},        {"Home", VK_HOME},
      {"End", VK_END},             {"PageUp", VK_PRIOR},          {"PageDown", VK_NEXT},
      {"Insert", VK_INSERT},       {"Delete", VK_DELETE},         {"Shift", VK_SHIFT},
      {"Control", VK_CONTROL},     {"Alt", VK_MENU},              {"Meta", VK_LWIN},
      {"F1", VK_F1},               {"F2", VK_F2},                 {"F3", VK_F3},
      {"F4", VK_F4},               {"F5", VK_F5},                 {"F6", VK_F6},
      {"F7", VK_F7},               {"F8", VK_F8},                 {"F9", VK_F9},
      {"F10", VK_F10},             {"F11", VK_F11},               {"F12", VK_F12},
  };

  const auto it = kKeyMap->find(key);
  if (it != kKeyMap->end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<WORD> ResolveWindowsVk(const json& payload) {
  const std::string code = PayloadCode(payload);
  if (!code.empty()) {
    const auto mapped = BrowserCodeToWindowsVk(code);
    if (mapped.has_value()) {
      return mapped;
    }
  }

  const std::string key = PayloadKey(payload);
  if (!key.empty()) {
    const auto mapped = BrowserKeyToWindowsVk(key);
    if (mapped.has_value()) {
      return mapped;
    }
  }

  const auto numeric = payload.find("key_code");
  if (numeric != payload.end() && numeric->is_number_integer()) {
    const int maybe_vk = numeric->get<int>();
    if (maybe_vk >= 0 && maybe_vk <= 255) {
      return static_cast<WORD>(maybe_vk);
    }
  }

  return std::nullopt;
}

bool SendWindowsKey(WORD vk, bool down) {
  INPUT input = {};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = vk;
  if (!down) {
    input.ki.dwFlags = KEYEVENTF_KEYUP;
  }
  return SendInput(1, &input, sizeof(INPUT)) == 1;
}

void EmitWindowsModifiers(const json& payload, bool down) {
  if (JsonBoolWithAliases(payload, {"shiftKey", "shift", "shift_key"})) {
    SendWindowsKey(VK_SHIFT, down);
  }
  if (JsonBoolWithAliases(payload, {"ctrlKey", "controlKey", "ctrl", "control", "ctrl_key"})) {
    SendWindowsKey(VK_CONTROL, down);
  }
  if (JsonBoolWithAliases(payload, {"altKey", "optionKey", "alt", "option", "alt_key"})) {
    SendWindowsKey(VK_MENU, down);
  }
  if (JsonBoolWithAliases(payload, {"metaKey", "commandKey", "meta", "command", "meta_key"})) {
    SendWindowsKey(VK_LWIN, down);
  }
}
#endif

}  // namespace

ScreenService::ScreenService() = default;

ScreenService::~ScreenService() {
  StopCapture();
}

std::string ScreenService::CapabilitiesJson() const {
  const bool supports_h264 = SupportsH264();
  const bool supports_h265 = SupportsH265();
  const bool supports_vp8 = SupportsVP8();
  const bool supports_vp9 = SupportsVP9();
  const bool supports_av1 = SupportsAV1();
  const bool supports_any_video = supports_h264 || supports_h265 || supports_vp8 || supports_vp9 || supports_av1;
#if defined(__APPLE__)
  return JsonCapabilities(supports_any_video, true, "screencapturekit", supports_h264, supports_h265,
                          supports_vp8, supports_vp9, supports_av1);
#elif defined(__linux__)
  return JsonCapabilities(supports_any_video, true, "x11", supports_h264, supports_h265,
                          supports_vp8, supports_vp9, supports_av1);
#elif defined(_WIN32)
  return JsonCapabilities(supports_any_video, true, "gdi", supports_h264, supports_h265,
                          supports_vp8, supports_vp9, supports_av1);
#else
  return JsonCapabilities(false, false, "unsupported", false, false, false, false, false);
#endif
}

std::vector<ScreenService::CaptureSource> ScreenService::ListCaptureSources(std::string* error) {
  std::vector<CaptureSource> sources;
#if defined(__APPLE__)
  if (!capture_bridge_) {
    capture_bridge_ = std::make_unique<ScreenCaptureKitBridge>();
  }
  if (!capture_bridge_) {
    if (error != nullptr) {
      *error = "screen capture bridge is not initialized";
    }
    return sources;
  }

  std::string list_error;
  auto displays = capture_bridge_->ListDisplays(&list_error);
  if (displays.empty()) {
    if (error != nullptr) {
      *error = list_error.empty() ? "no capture display available" : list_error;
    }
    return sources;
  }

  sources.reserve(displays.size());
  for (const auto& display : displays) {
    CaptureSource source;
    source.id = display.id;
    source.name = display.name;
    source.width = display.width;
    source.height = display.height;
    source.is_default = display.is_default;
    sources.push_back(std::move(source));
  }
  return sources;
#elif defined(__linux__)
  Display* display = XOpenDisplay(nullptr);
  if (display == nullptr) {
    if (error != nullptr) {
      *error = "failed to open X11 display";
    }
    return sources;
  }

  const int screen = DefaultScreen(display);
  const int width = DisplayWidth(display, screen);
  const int height = DisplayHeight(display, screen);
  XCloseDisplay(display);

  CaptureSource source;
  source.id = "default";
  source.width = std::max(width, 0);
  source.height = std::max(height, 0);
  source.is_default = true;
  source.name = "Display 1 (" + std::to_string(source.width) + "x" + std::to_string(source.height) + ")";
  sources.push_back(std::move(source));
  return sources;
#elif defined(_WIN32)
  int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (width <= 0 || height <= 0) {
    width = GetSystemMetrics(SM_CXSCREEN);
    height = GetSystemMetrics(SM_CYSCREEN);
  }

  CaptureSource source;
  source.id = "default";
  source.width = std::max(width, 0);
  source.height = std::max(height, 0);
  source.is_default = true;
  source.name = "Display 1 (" + std::to_string(source.width) + "x" + std::to_string(source.height) + ")";
  sources.push_back(std::move(source));
  return sources;
#else
  if (error != nullptr) {
    *error = "native capture is unsupported on this platform";
  }
  return sources;
#endif
}

std::string ScreenService::NormalizeCaptureSourceId(const std::string& requested_source_id,
                                                    std::string* error) {
  std::string list_error;
  const auto sources = ListCaptureSources(&list_error);
  if (sources.empty()) {
    if (error != nullptr) {
      *error = list_error.empty() ? "no capture source available" : list_error;
    }
    return "";
  }

  if (requested_source_id.empty()) {
    for (const auto& source : sources) {
      if (source.is_default && !source.id.empty()) {
        return source.id;
      }
    }
    return sources.front().id;
  }

  for (const auto& source : sources) {
    if (source.id == requested_source_id) {
      return source.id;
    }
  }

  for (const auto& source : sources) {
    if (source.is_default && !source.id.empty()) {
      return source.id;
    }
  }
  return sources.front().id;
}

std::string ScreenService::ActiveCaptureSourceId() const {
  std::lock_guard<std::mutex> lock(capture_source_mu_);
  return active_capture_source_id_;
}

bool ScreenService::SupportsH264() const {
  return SupportsVideoCodec(VideoCodec::kH264);
}

bool ScreenService::SupportsH265() const {
  return SupportsVideoCodec(VideoCodec::kH265);
}

bool ScreenService::SupportsVP8() const {
  return SupportsVideoCodec(VideoCodec::kVP8);
}

bool ScreenService::SupportsVP9() const {
  return SupportsVideoCodec(VideoCodec::kVP9);
}

bool ScreenService::SupportsAV1() const {
  return SupportsVideoCodec(VideoCodec::kAV1);
}

void ScreenService::SetEncodingTargets(bool enable_jpeg, bool enable_h264, bool enable_h265, bool enable_vp8,
                                       bool enable_vp9, bool enable_av1) {
  encode_jpeg_.store(enable_jpeg);
  encode_h264_.store(enable_h264);
  encode_h265_.store(enable_h265);
  encode_vp8_.store(enable_vp8);
  encode_vp9_.store(enable_vp9);
  encode_av1_.store(enable_av1);
}

void ScreenService::SetEncodingProfile(int scale_percent, int video_bitrate_bps) {
  capture_scale_percent_.store(std::clamp(scale_percent, 40, 100));
  video_bitrate_bps_.store(std::clamp(video_bitrate_bps, 500'000, 12'000'000));
}

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

bool ScreenService::InjectInputEvent(const std::string& session_token, const InputEvent& event,
                                     std::string* error) {
  if (!CanInjectInput(session_token)) {
    if (error != nullptr) {
      *error = "remote control is not authorized";
    }
    return false;
  }

  return InjectInputEventNative(event, error);
}

bool ScreenService::StartCapture(int fps, const std::string& source_id, std::string* error) {
#if !defined(__APPLE__) && !defined(__linux__) && !defined(_WIN32)
  (void)fps;
  (void)source_id;
  if (error != nullptr) {
    *error = "native capture is unsupported on this platform";
  }
  return false;
#else
  if (!encode_jpeg_.load() && !encode_h264_.load() && !encode_h265_.load() && !encode_vp8_.load() &&
      !encode_vp9_.load() && !encode_av1_.load()) {
    if (error != nullptr) {
      *error = "native capture has no active encoding targets";
    }
    return false;
  }

  std::string source_error;
  const std::string normalized_source_id = NormalizeCaptureSourceId(source_id, &source_error);
  if (normalized_source_id.empty()) {
    if (error != nullptr) {
      *error = source_error.empty() ? "no capture source available" : source_error;
    }
    return false;
  }

  if (capture_running_.exchange(true)) {
    return true;
  }

  const int safe_fps = std::clamp(fps, 1, 60);
  capture_fps_ = safe_fps;
  if (encode_h264_.load() && !h264_encoder_) {
    h264_encoder_ = CreateVideoEncoder(VideoCodec::kH264);
  }
  if (encode_h265_.load() && !h265_encoder_) {
    h265_encoder_ = CreateVideoEncoder(VideoCodec::kH265);
  }
  if (encode_vp8_.load() && !vp8_encoder_) {
    vp8_encoder_ = CreateVideoEncoder(VideoCodec::kVP8);
  }
  if (encode_vp9_.load() && !vp9_encoder_) {
    vp9_encoder_ = CreateVideoEncoder(VideoCodec::kVP9);
  }
  if (encode_av1_.load() && !av1_encoder_) {
    av1_encoder_ = CreateVideoEncoder(VideoCodec::kAV1);
  }

#if !FERRYMAN_WITH_FFMPEG
  capture_running_ = false;
  if (error != nullptr) {
    *error = "native capture requires ffmpeg (libavcodec/libswscale/libavutil)";
  }
  return false;
#endif

#if defined(__APPLE__)
  if (!CGPreflightScreenCaptureAccess() && !CGRequestScreenCaptureAccess()) {
    capture_running_ = false;
    if (error != nullptr) {
      *error = "screen capture permission denied by macOS";
    }
    return false;
  }

  if (!capture_bridge_) {
    capture_bridge_ = std::make_unique<ScreenCaptureKitBridge>();
  }

  if (!capture_bridge_->Start(safe_fps, normalized_source_id, error)) {
    capture_running_ = false;
    return false;
  }
#endif

  {
    std::lock_guard<std::mutex> lock(capture_source_mu_);
    active_capture_source_id_ = normalized_source_id;
  }

  capture_thread_ = std::thread([this, safe_fps]() {
    CaptureLoop(safe_fps);
  });
  return true;
#endif
}

void ScreenService::StopCapture() {
  capture_running_ = false;
#if defined(__APPLE__)
  if (capture_bridge_) {
    capture_bridge_->Stop();
  }
#endif
  if (capture_thread_.joinable()) {
    capture_thread_.join();
  }
  if (h264_encoder_) {
    h264_encoder_->Reset();
  }
  if (h265_encoder_) {
    h265_encoder_->Reset();
  }
  if (vp8_encoder_) {
    vp8_encoder_->Reset();
  }
  if (vp9_encoder_) {
    vp9_encoder_->Reset();
  }
  if (av1_encoder_) {
    av1_encoder_->Reset();
  }
  {
    std::lock_guard<std::mutex> lock(capture_source_mu_);
    active_capture_source_id_.clear();
  }
}

std::optional<ScreenService::EncodedFrame> ScreenService::LatestFrame() const {
  std::lock_guard<std::mutex> lock(frame_mu_);
  return latest_frame_;
}

void ScreenService::CaptureLoop(int fps) {
  const auto retry_interval = std::chrono::milliseconds(std::max(1000 / std::max(1, fps), 8));
  uint64_t seq = 0;

  while (capture_running_) {
    EncodedFrame frame;
    std::string error;
    if (CaptureFrame(&frame, &error)) {
      frame.sequence = ++seq;
      frame.captured_at_ms = EpochMillisNow();
      std::lock_guard<std::mutex> lock(frame_mu_);
      latest_frame_ = std::move(frame);
      continue;
    }
    std::this_thread::sleep_for(retry_interval);
  }
}

bool ScreenService::CaptureFrame(EncodedFrame* frame, std::string* error) {
  if (frame == nullptr) {
    if (error != nullptr) {
      *error = "frame output is null";
    }
    return false;
  }

  const bool want_jpeg = encode_jpeg_.load();
  const bool want_h264 = encode_h264_.load();
  const bool want_h265 = encode_h265_.load();
  const bool want_vp8 = encode_vp8_.load();
  const bool want_vp9 = encode_vp9_.load();
  const bool want_av1 = encode_av1_.load();
  const bool want_video = want_h264 || want_h265 || want_vp8 || want_vp9 || want_av1;
  const int scale_percent = std::clamp(capture_scale_percent_.load(), 40, 100);
  const int target_video_bitrate_bps = std::clamp(video_bitrate_bps_.load(), 500'000, 12'000'000);
  if (!want_jpeg && !want_video) {
    if (error != nullptr) {
      *error = "native capture has no active encoding targets";
    }
    return false;
  }

#if defined(__APPLE__)
  if (!capture_bridge_) {
    if (error != nullptr) {
      *error = "screen capture bridge is not initialized";
    }
    return false;
  }

  ScreenCaptureKitBridge::RawFrame raw_frame;
  if (!capture_bridge_->WaitForFrame(1200, &raw_frame, error)) {
    return false;
  }

  if (raw_frame.width <= 0 || raw_frame.height <= 0 ||
      raw_frame.stride_bytes <= 0 || raw_frame.bgra_bytes.empty()) {
    if (error != nullptr) {
      *error = "screen capture produced empty bgra frame";
    }
    return false;
  }

#if FERRYMAN_WITH_FFMPEG
  int encode_width = std::max(2, (raw_frame.width * scale_percent) / 100);
  int encode_height = std::max(2, (raw_frame.height * scale_percent) / 100);
  if (want_video) {
    if ((encode_width & 1) != 0 && encode_width > 2) {
      encode_width -= 1;
    }
    if ((encode_height & 1) != 0 && encode_height > 2) {
      encode_height -= 1;
    }
  }

  const uint8_t* encode_source = reinterpret_cast<const uint8_t*>(raw_frame.bgra_bytes.data());
  int encode_stride = raw_frame.stride_bytes;
  std::string scaled_bgra;
  if (encode_width != raw_frame.width || encode_height != raw_frame.height) {
    std::string scale_error;
    if (!ScaleFrameWithFfmpeg(encode_source, raw_frame.width, raw_frame.height, raw_frame.stride_bytes,
                              AV_PIX_FMT_BGRA, encode_width, encode_height, &scaled_bgra, &encode_stride,
                              &scale_error)) {
      if (error != nullptr) {
        *error = scale_error.empty() ? "failed to scale bgra frame" : scale_error;
      }
      return false;
    }
    encode_source = reinterpret_cast<const uint8_t*>(scaled_bgra.data());
  }

  frame->width = encode_width;
  frame->height = encode_height;
  bool produced_any = false;
  std::string first_error;
  auto remember_error = [&first_error](const std::string& candidate) {
    if (first_error.empty() && !candidate.empty()) {
      first_error = candidate;
    }
  };

  if (want_jpeg) {
    if (!raw_frame.jpeg_bytes.empty() && encode_width == raw_frame.width && encode_height == raw_frame.height) {
      frame->jpeg_bytes = raw_frame.jpeg_bytes;
      produced_any = true;
    } else {
      std::string jpeg_bytes;
      std::string jpeg_error;
      if (EncodeFrameToJpegFfmpeg(
              encode_source,
              encode_width,
              encode_height,
              encode_stride,
              AV_PIX_FMT_BGRA,
              &jpeg_bytes,
              &jpeg_error)) {
        frame->jpeg_bytes = std::move(jpeg_bytes);
        produced_any = true;
      } else {
        remember_error(jpeg_error);
      }
    }
  }

  auto encode_video = [&](VideoCodec codec, std::unique_ptr<VideoEncoder>* encoder_slot,
                          std::string* encoded_bytes, bool* encoded_keyframe) {
    if (!(*encoder_slot)) {
      *encoder_slot = CreateVideoEncoder(codec);
    }
    std::string encode_error;
    if ((*encoder_slot)->EnsureConfigured(encode_width, encode_height, capture_fps_, target_video_bitrate_bps,
                                          static_cast<int>(AV_PIX_FMT_BGRA), &encode_error)) {
      std::string encoded;
      bool keyframe = false;
      if ((*encoder_slot)->EncodeFrame(encode_source, encode_stride, &encoded, &keyframe, &encode_error)) {
        *encoded_bytes = std::move(encoded);
        *encoded_keyframe = keyframe;
        produced_any = true;
      } else {
        remember_error(encode_error);
      }
    } else {
      remember_error(encode_error);
    }
  };

  if (want_h264) {
    encode_video(VideoCodec::kH264, &h264_encoder_, &frame->h264_bytes, &frame->h264_keyframe);
  }
  if (want_h265) {
    encode_video(VideoCodec::kH265, &h265_encoder_, &frame->h265_bytes, &frame->h265_keyframe);
  }
  if (want_vp8) {
    encode_video(VideoCodec::kVP8, &vp8_encoder_, &frame->vp8_bytes, &frame->vp8_keyframe);
  }
  if (want_vp9) {
    encode_video(VideoCodec::kVP9, &vp9_encoder_, &frame->vp9_bytes, &frame->vp9_keyframe);
  }
  if (want_av1) {
    encode_video(VideoCodec::kAV1, &av1_encoder_, &frame->av1_bytes, &frame->av1_keyframe);
  }

  if (!produced_any) {
    if (error != nullptr) {
      *error = first_error.empty() ? "failed to encode frame for requested targets" : first_error;
    }
    return false;
  }
#else
  if (error != nullptr) {
    *error = "macOS native capture requires ffmpeg";
  }
  return false;
#endif

  return true;
#elif defined(__linux__)
#if !FERRYMAN_WITH_FFMPEG
  if (error != nullptr) {
    *error = "linux native capture requires ffmpeg encoder";
  }
  return false;
#else
  Display* display = XOpenDisplay(nullptr);
  if (display == nullptr) {
    if (error != nullptr) {
      *error = "failed to open X11 display";
    }
    return false;
  }

  const int screen = DefaultScreen(display);
  const Window root = RootWindow(display, screen);
  XWindowAttributes attrs{};
  if (XGetWindowAttributes(display, root, &attrs) == 0 || attrs.width <= 0 || attrs.height <= 0) {
    XCloseDisplay(display);
    if (error != nullptr) {
      *error = "failed to read X11 root window attributes";
    }
    return false;
  }

  XImage* image = XGetImage(display, root, 0, 0,
                            static_cast<unsigned int>(attrs.width),
                            static_cast<unsigned int>(attrs.height),
                            AllPlanes, ZPixmap);
  if (image == nullptr) {
    XCloseDisplay(display);
    if (error != nullptr) {
      *error = "failed to capture X11 root window image";
    }
    return false;
  }

  const int width = attrs.width;
  const int height = attrs.height;
  std::vector<uint8_t> bgr(static_cast<size_t>(width) * static_cast<size_t>(height) * 3ULL);

  size_t out_index = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const unsigned long pixel = XGetPixel(image, x, y);
      const uint8_t r = NormalizeMaskValue(pixel, image->red_mask);
      const uint8_t g = NormalizeMaskValue(pixel, image->green_mask);
      const uint8_t b = NormalizeMaskValue(pixel, image->blue_mask);
      bgr[out_index++] = b;
      bgr[out_index++] = g;
      bgr[out_index++] = r;
    }
  }

  XDestroyImage(image);
  XCloseDisplay(display);

  int encode_width = std::max(2, (width * scale_percent) / 100);
  int encode_height = std::max(2, (height * scale_percent) / 100);
  if (want_video) {
    if ((encode_width & 1) != 0 && encode_width > 2) {
      encode_width -= 1;
    }
    if ((encode_height & 1) != 0 && encode_height > 2) {
      encode_height -= 1;
    }
  }

  const uint8_t* encode_source = bgr.data();
  int encode_stride = width * 3;
  std::string scaled_bgr;
  if (encode_width != width || encode_height != height) {
    std::string scale_error;
    if (!ScaleFrameWithFfmpeg(encode_source, width, height, width * 3, AV_PIX_FMT_BGR24,
                              encode_width, encode_height, &scaled_bgr, &encode_stride, &scale_error)) {
      if (error != nullptr) {
        *error = scale_error.empty() ? "failed to scale bgr frame" : scale_error;
      }
      return false;
    }
    encode_source = reinterpret_cast<const uint8_t*>(scaled_bgr.data());
  }

  frame->width = encode_width;
  frame->height = encode_height;
  bool produced_any = false;
  std::string first_error;
  auto remember_error = [&first_error](const std::string& candidate) {
    if (first_error.empty() && !candidate.empty()) {
      first_error = candidate;
    }
  };

  if (want_jpeg) {
    std::string jpeg;
    std::string jpeg_error;
    if (EncodeFrameToJpegFfmpeg(encode_source, encode_width, encode_height, encode_stride, AV_PIX_FMT_BGR24, &jpeg,
                                &jpeg_error)) {
      frame->jpeg_bytes = std::move(jpeg);
      produced_any = true;
    } else {
      remember_error(jpeg_error);
    }
  }

  auto encode_video = [&](VideoCodec codec, std::unique_ptr<VideoEncoder>* encoder_slot,
                          std::string* encoded_bytes, bool* encoded_keyframe) {
    if (!(*encoder_slot)) {
      *encoder_slot = CreateVideoEncoder(codec);
    }
    std::string encode_error;
    if ((*encoder_slot)->EnsureConfigured(encode_width, encode_height, capture_fps_, target_video_bitrate_bps,
                                          static_cast<int>(AV_PIX_FMT_BGR24), &encode_error)) {
      std::string encoded;
      bool keyframe = false;
      if ((*encoder_slot)->EncodeFrame(encode_source, encode_stride, &encoded, &keyframe, &encode_error)) {
        *encoded_bytes = std::move(encoded);
        *encoded_keyframe = keyframe;
        produced_any = true;
      } else {
        remember_error(encode_error);
      }
    } else {
      remember_error(encode_error);
    }
  };

  if (want_h264) {
    encode_video(VideoCodec::kH264, &h264_encoder_, &frame->h264_bytes, &frame->h264_keyframe);
  }
  if (want_h265) {
    encode_video(VideoCodec::kH265, &h265_encoder_, &frame->h265_bytes, &frame->h265_keyframe);
  }
  if (want_vp8) {
    encode_video(VideoCodec::kVP8, &vp8_encoder_, &frame->vp8_bytes, &frame->vp8_keyframe);
  }
  if (want_vp9) {
    encode_video(VideoCodec::kVP9, &vp9_encoder_, &frame->vp9_bytes, &frame->vp9_keyframe);
  }
  if (want_av1) {
    encode_video(VideoCodec::kAV1, &av1_encoder_, &frame->av1_bytes, &frame->av1_keyframe);
  }

  if (!produced_any) {
    if (error != nullptr) {
      *error = first_error.empty() ? "failed to encode frame for requested targets" : first_error;
    }
    return false;
  }
  return true;
#endif
#elif defined(_WIN32)
#if !FERRYMAN_WITH_FFMPEG
  if (error != nullptr) {
    *error = "windows native capture requires ffmpeg encoder";
  }
  return false;
#else
  const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (width <= 0 || height <= 0) {
    if (error != nullptr) {
      *error = "invalid virtual screen size";
    }
    return false;
  }

  HDC screen_dc = GetDC(nullptr);
  if (screen_dc == nullptr) {
    if (error != nullptr) {
      *error = "failed to get screen device context";
    }
    return false;
  }

  HDC memory_dc = CreateCompatibleDC(screen_dc);
  if (memory_dc == nullptr) {
    ReleaseDC(nullptr, screen_dc);
    if (error != nullptr) {
      *error = "failed to create memory device context";
    }
    return false;
  }

  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* dib_pixels = nullptr;
  HBITMAP bitmap = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &dib_pixels, nullptr, 0);
  if (bitmap == nullptr || dib_pixels == nullptr) {
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    if (error != nullptr) {
      *error = "failed to create DIB section";
    }
    return false;
  }

  HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
  if (!BitBlt(memory_dc, 0, 0, width, height, screen_dc, left, top, SRCCOPY | CAPTUREBLT)) {
    SelectObject(memory_dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    if (error != nullptr) {
      *error = "BitBlt screen capture failed";
    }
    return false;
  }

  const uint8_t* bgra = static_cast<const uint8_t*>(dib_pixels);
  std::vector<uint8_t> bgr(static_cast<size_t>(width) * static_cast<size_t>(height) * 3ULL);
  size_t out_index = 0;
  const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  for (size_t idx = 0; idx < pixels; ++idx) {
    const size_t in_index = idx * 4ULL;
    bgr[out_index++] = bgra[in_index + 0];
    bgr[out_index++] = bgra[in_index + 1];
    bgr[out_index++] = bgra[in_index + 2];
  }

  SelectObject(memory_dc, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(memory_dc);
  ReleaseDC(nullptr, screen_dc);

  int encode_width = std::max(2, (width * scale_percent) / 100);
  int encode_height = std::max(2, (height * scale_percent) / 100);
  if (want_video) {
    if ((encode_width & 1) != 0 && encode_width > 2) {
      encode_width -= 1;
    }
    if ((encode_height & 1) != 0 && encode_height > 2) {
      encode_height -= 1;
    }
  }

  const uint8_t* encode_source = bgr.data();
  int encode_stride = width * 3;
  std::string scaled_bgr;
  if (encode_width != width || encode_height != height) {
    std::string scale_error;
    if (!ScaleFrameWithFfmpeg(encode_source, width, height, width * 3, AV_PIX_FMT_BGR24,
                              encode_width, encode_height, &scaled_bgr, &encode_stride, &scale_error)) {
      if (error != nullptr) {
        *error = scale_error.empty() ? "failed to scale bgr frame" : scale_error;
      }
      return false;
    }
    encode_source = reinterpret_cast<const uint8_t*>(scaled_bgr.data());
  }

  frame->width = encode_width;
  frame->height = encode_height;
  bool produced_any = false;
  std::string first_error;
  auto remember_error = [&first_error](const std::string& candidate) {
    if (first_error.empty() && !candidate.empty()) {
      first_error = candidate;
    }
  };

  if (want_jpeg) {
    std::string jpeg;
    std::string jpeg_error;
    if (EncodeFrameToJpegFfmpeg(encode_source, encode_width, encode_height, encode_stride, AV_PIX_FMT_BGR24, &jpeg,
                                &jpeg_error)) {
      frame->jpeg_bytes = std::move(jpeg);
      produced_any = true;
    } else {
      remember_error(jpeg_error);
    }
  }

  auto encode_video = [&](VideoCodec codec, std::unique_ptr<VideoEncoder>* encoder_slot,
                          std::string* encoded_bytes, bool* encoded_keyframe) {
    if (!(*encoder_slot)) {
      *encoder_slot = CreateVideoEncoder(codec);
    }
    std::string encode_error;
    if ((*encoder_slot)->EnsureConfigured(encode_width, encode_height, capture_fps_, target_video_bitrate_bps,
                                          static_cast<int>(AV_PIX_FMT_BGR24), &encode_error)) {
      std::string encoded;
      bool keyframe = false;
      if ((*encoder_slot)->EncodeFrame(encode_source, encode_stride, &encoded, &keyframe, &encode_error)) {
        *encoded_bytes = std::move(encoded);
        *encoded_keyframe = keyframe;
        produced_any = true;
      } else {
        remember_error(encode_error);
      }
    } else {
      remember_error(encode_error);
    }
  };

  if (want_h264) {
    encode_video(VideoCodec::kH264, &h264_encoder_, &frame->h264_bytes, &frame->h264_keyframe);
  }
  if (want_h265) {
    encode_video(VideoCodec::kH265, &h265_encoder_, &frame->h265_bytes, &frame->h265_keyframe);
  }
  if (want_vp8) {
    encode_video(VideoCodec::kVP8, &vp8_encoder_, &frame->vp8_bytes, &frame->vp8_keyframe);
  }
  if (want_vp9) {
    encode_video(VideoCodec::kVP9, &vp9_encoder_, &frame->vp9_bytes, &frame->vp9_keyframe);
  }
  if (want_av1) {
    encode_video(VideoCodec::kAV1, &av1_encoder_, &frame->av1_bytes, &frame->av1_keyframe);
  }

  if (!produced_any) {
    if (error != nullptr) {
      *error = first_error.empty() ? "failed to encode frame for requested targets" : first_error;
    }
    return false;
  }
  return true;
#endif
#else
  if (error != nullptr) {
    *error = "native capture is unsupported on this platform";
  }
  return false;
#endif
}

bool ScreenService::InjectInputEventNative(const InputEvent& event, std::string* error) {
#if defined(__APPLE__)
  if (!AXIsProcessTrusted()) {
    if (error != nullptr) {
      *error = "accessibility permission denied. grant trust in System Settings > Privacy & Security > Accessibility";
    }
    return false;
  }

  json payload = json::object();
  if (!event.payload.empty()) {
    payload = json::parse(event.payload, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
      if (error != nullptr) {
        *error = "payload must be a valid json object";
      }
      return false;
    }
  }

  double current_x = 0.0;
  double current_y = 0.0;
  {
    std::lock_guard<std::mutex> lock(pointer_mu_);
    current_x = last_pointer_x_;
    current_y = last_pointer_y_;
  }

  if (event.type == "mouse_move") {
    const CGPoint point = NormalizePointToMainDisplay(payload, current_x, current_y, &current_x, &current_y);
    CGEventRef move = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, point, kCGMouseButtonLeft);
    if (move == nullptr) {
      if (error != nullptr) {
        *error = "failed to create mouse move event";
      }
      return false;
    }
    CGEventPost(kCGHIDEventTap, move);
    CFRelease(move);

    {
      std::lock_guard<std::mutex> lock(pointer_mu_);
      last_pointer_x_ = current_x;
      last_pointer_y_ = current_y;
    }

    if (error != nullptr) {
      *error = "mouse move injected";
    }
    return true;
  }

  if (event.type == "mouse_click") {
    const int button_code = payload.value("button", 0);
    const CGMouseButton button = ParseMouseButtonMac(button_code);
    const auto [down_type, up_type] = MouseDownUpTypeMac(button);
    const CGPoint point = CGPointMake(current_x, current_y);

    CGEventRef down = CGEventCreateMouseEvent(nullptr, down_type, point, button);
    CGEventRef up = CGEventCreateMouseEvent(nullptr, up_type, point, button);
    if (down == nullptr || up == nullptr) {
      if (down != nullptr) {
        CFRelease(down);
      }
      if (up != nullptr) {
        CFRelease(up);
      }
      if (error != nullptr) {
        *error = "failed to create mouse click event";
      }
      return false;
    }

    CGEventPost(kCGHIDEventTap, down);
    CGEventPost(kCGHIDEventTap, up);
    CFRelease(down);
    CFRelease(up);

    if (error != nullptr) {
      *error = "mouse click injected";
    }
    return true;
  }

  if (event.type == "mouse_wheel") {
    const int delta_y = payload.value("delta_y", 0);
    CGEventRef wheel = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 1, delta_y);
    if (wheel == nullptr) {
      if (error != nullptr) {
        *error = "failed to create wheel event";
      }
      return false;
    }

    CGEventPost(kCGHIDEventTap, wheel);
    CFRelease(wheel);

    if (error != nullptr) {
      *error = "mouse wheel injected";
    }
    return true;
  }

  if (event.type == "key_down" || event.type == "key_up" || event.type == "key_tap") {
    const auto maybe_key_code = ResolveMacKeyCode(payload);
    if (!maybe_key_code.has_value()) {
      if (error != nullptr) {
        *error = "unable to resolve browser key to macOS keycode";
      }
      return false;
    }

    const auto key_code = static_cast<CGKeyCode>(*maybe_key_code);
    const CGEventFlags flags = ModifierFlagsFromPayloadMac(payload);

    if (event.type == "key_tap") {
      CGEventRef down = CGEventCreateKeyboardEvent(nullptr, key_code, true);
      CGEventRef up = CGEventCreateKeyboardEvent(nullptr, key_code, false);
      if (down == nullptr || up == nullptr) {
        if (down != nullptr) {
          CFRelease(down);
        }
        if (up != nullptr) {
          CFRelease(up);
        }
        if (error != nullptr) {
          *error = "failed to create keyboard tap event";
        }
        return false;
      }
      CGEventSetFlags(down, flags);
      CGEventSetFlags(up, flags);
      CGEventPost(kCGHIDEventTap, down);
      CGEventPost(kCGHIDEventTap, up);
      CFRelease(down);
      CFRelease(up);
    } else {
      const bool key_down = event.type == "key_down";
      CGEventRef key_event = CGEventCreateKeyboardEvent(nullptr, key_code, key_down);
      if (key_event == nullptr) {
        if (error != nullptr) {
          *error = "failed to create keyboard event";
        }
        return false;
      }
      CGEventSetFlags(key_event, flags);
      CGEventPost(kCGHIDEventTap, key_event);
      CFRelease(key_event);
    }

    if (error != nullptr) {
      *error = "keyboard event injected";
    }
    return true;
  }

  if (error != nullptr) {
    *error = "unsupported input event type: " + event.type;
  }
  return false;
#elif defined(__linux__)
  Display* display = XOpenDisplay(nullptr);
  if (display == nullptr) {
    if (error != nullptr) {
      *error = "failed to open X11 display for input injection";
    }
    return false;
  }

  json payload = json::object();
  if (!event.payload.empty()) {
    payload = json::parse(event.payload, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
      XCloseDisplay(display);
      if (error != nullptr) {
        *error = "payload must be a valid json object";
      }
      return false;
    }
  }

  const int screen = DefaultScreen(display);
  const double screen_w = static_cast<double>(DisplayWidth(display, screen));
  const double screen_h = static_cast<double>(DisplayHeight(display, screen));

  double current_x = 0.0;
  double current_y = 0.0;
  {
    std::lock_guard<std::mutex> lock(pointer_mu_);
    current_x = last_pointer_x_;
    current_y = last_pointer_y_;
  }

  if (event.type == "mouse_move") {
    const double input_x = payload.value("x", current_x);
    const double input_y = payload.value("y", current_y);
    const double view_w = payload.value("width", screen_w);
    const double view_h = payload.value("height", screen_h);

    if (view_w > 1.0 && view_h > 1.0) {
      current_x = std::clamp(input_x / view_w, 0.0, 1.0) * screen_w;
      current_y = std::clamp(input_y / view_h, 0.0, 1.0) * screen_h;
    } else {
      current_x = input_x;
      current_y = input_y;
    }

    XTestFakeMotionEvent(display, -1, static_cast<int>(current_x), static_cast<int>(current_y), CurrentTime);
    XFlush(display);

    {
      std::lock_guard<std::mutex> lock(pointer_mu_);
      last_pointer_x_ = current_x;
      last_pointer_y_ = current_y;
    }

    XCloseDisplay(display);
    if (error != nullptr) {
      *error = "mouse move injected";
    }
    return true;
  }

  if (event.type == "mouse_click") {
    const int button = LinuxButtonFromBrowserButton(payload.value("button", 0));
    XTestFakeButtonEvent(display, button, True, CurrentTime);
    XTestFakeButtonEvent(display, button, False, CurrentTime);
    XFlush(display);
    XCloseDisplay(display);

    if (error != nullptr) {
      *error = "mouse click injected";
    }
    return true;
  }

  if (event.type == "mouse_wheel") {
    const int delta_y = payload.value("delta_y", 0);
    const int button = delta_y > 0 ? 5 : 4;
    const int steps = std::max(1, std::abs(delta_y) / 120 + 1);
    for (int i = 0; i < steps; ++i) {
      XTestFakeButtonEvent(display, button, True, CurrentTime);
      XTestFakeButtonEvent(display, button, False, CurrentTime);
    }
    XFlush(display);
    XCloseDisplay(display);

    if (error != nullptr) {
      *error = "mouse wheel injected";
    }
    return true;
  }

  if (event.type == "key_down" || event.type == "key_up" || event.type == "key_tap") {
    const auto key_sym = ResolveLinuxKeySym(payload);
    if (!key_sym.has_value()) {
      XCloseDisplay(display);
      if (error != nullptr) {
        *error = "unable to resolve browser key to X11 keysym";
      }
      return false;
    }

    const KeyCode code = XKeysymToKeycode(display, *key_sym);
    if (code == 0) {
      XCloseDisplay(display);
      if (error != nullptr) {
        *error = "unable to resolve X11 keycode";
      }
      return false;
    }

    if (event.type == "key_tap") {
      EmitLinuxPayloadModifiers(display, payload, true);
      XTestFakeKeyEvent(display, code, True, CurrentTime);
      XTestFakeKeyEvent(display, code, False, CurrentTime);
      EmitLinuxPayloadModifiers(display, payload, false);
    } else {
      XTestFakeKeyEvent(display, code, event.type == "key_down" ? True : False, CurrentTime);
    }

    XFlush(display);
    XCloseDisplay(display);
    if (error != nullptr) {
      *error = "keyboard event injected";
    }
    return true;
  }

  XCloseDisplay(display);
  if (error != nullptr) {
    *error = "unsupported input event type: " + event.type;
  }
  return false;
#elif defined(_WIN32)
  json payload = json::object();
  if (!event.payload.empty()) {
    payload = json::parse(event.payload, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
      if (error != nullptr) {
        *error = "payload must be a valid json object";
      }
      return false;
    }
  }

  double current_x = 0.0;
  double current_y = 0.0;
  {
    std::lock_guard<std::mutex> lock(pointer_mu_);
    current_x = last_pointer_x_;
    current_y = last_pointer_y_;
  }

  const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const double screen_w = static_cast<double>(GetSystemMetrics(SM_CXVIRTUALSCREEN));
  const double screen_h = static_cast<double>(GetSystemMetrics(SM_CYVIRTUALSCREEN));

  if (event.type == "mouse_move") {
    const double input_x = payload.value("x", current_x);
    const double input_y = payload.value("y", current_y);
    const double view_w = payload.value("width", screen_w);
    const double view_h = payload.value("height", screen_h);

    if (view_w > 1.0 && view_h > 1.0) {
      current_x = left + std::clamp(input_x / view_w, 0.0, 1.0) * screen_w;
      current_y = top + std::clamp(input_y / view_h, 0.0, 1.0) * screen_h;
    } else {
      current_x = input_x;
      current_y = input_y;
    }

    if (!SetCursorPos(static_cast<int>(current_x), static_cast<int>(current_y))) {
      if (error != nullptr) {
        *error = "SetCursorPos failed";
      }
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(pointer_mu_);
      last_pointer_x_ = current_x;
      last_pointer_y_ = current_y;
    }

    if (error != nullptr) {
      *error = "mouse move injected";
    }
    return true;
  }

  if (event.type == "mouse_click") {
    const int browser_button = payload.value("button", 0);

    INPUT down = {};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = WindowsButtonDownFlag(browser_button);

    INPUT up = {};
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = WindowsButtonUpFlag(browser_button);

    INPUT inputs[2] = {down, up};
    if (SendInput(2, inputs, sizeof(INPUT)) != 2) {
      if (error != nullptr) {
        *error = "SendInput mouse click failed";
      }
      return false;
    }

    if (error != nullptr) {
      *error = "mouse click injected";
    }
    return true;
  }

  if (event.type == "mouse_wheel") {
    const int delta_y = payload.value("delta_y", 0);

    INPUT wheel = {};
    wheel.type = INPUT_MOUSE;
    wheel.mi.dwFlags = MOUSEEVENTF_WHEEL;
    wheel.mi.mouseData = static_cast<DWORD>(-delta_y);

    if (SendInput(1, &wheel, sizeof(INPUT)) != 1) {
      if (error != nullptr) {
        *error = "SendInput mouse wheel failed";
      }
      return false;
    }

    if (error != nullptr) {
      *error = "mouse wheel injected";
    }
    return true;
  }

  if (event.type == "key_down" || event.type == "key_up" || event.type == "key_tap") {
    const auto vk = ResolveWindowsVk(payload);
    if (!vk.has_value()) {
      if (error != nullptr) {
        *error = "unable to resolve browser key to Windows virtual key";
      }
      return false;
    }

    if (event.type == "key_tap") {
      EmitWindowsModifiers(payload, true);
      const bool down_ok = SendWindowsKey(*vk, true);
      const bool up_ok = SendWindowsKey(*vk, false);
      EmitWindowsModifiers(payload, false);
      if (!down_ok || !up_ok) {
        if (error != nullptr) {
          *error = "SendInput key tap failed";
        }
        return false;
      }
    } else {
      if (!SendWindowsKey(*vk, event.type == "key_down")) {
        if (error != nullptr) {
          *error = "SendInput keyboard event failed";
        }
        return false;
      }
    }

    if (error != nullptr) {
      *error = "keyboard event injected";
    }
    return true;
  }

  if (error != nullptr) {
    *error = "unsupported input event type: " + event.type;
  }
  return false;
#else
  (void)event;
  if (error != nullptr) {
    *error = "input injection is unsupported on this platform";
  }
  return false;
#endif
}

}  // namespace ferryman::web
