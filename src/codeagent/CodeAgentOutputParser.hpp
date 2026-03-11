#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace ferryman::codeagent::parser {

struct AgentOutputParseState {
  std::unordered_map<std::string, std::string> agent_message_buffers;
  std::unordered_map<std::string, std::string> reasoning_buffers;
  std::unordered_map<std::string, std::string> command_output_buffers;
  std::unordered_map<std::string, std::string> agent_message_stream_keys;
  std::unordered_map<std::string, std::string> reasoning_stream_keys;
  std::unordered_map<int, std::string> claude_block_types;
  std::unordered_map<std::string, nlohmann::json> command_meta;
  std::unordered_map<std::string, nlohmann::json> patch_meta;
  std::string claude_active_message_id;
  std::string live_reasoning_buffer;
  bool reasoning_tool_started = false;
  std::string reasoning_tool_call_id;
  std::string reasoning_title;
  std::string last_diff;
};

nlohmann::json CodexMessageBody(const std::string& text);

bool ParseAgentOutputJson(const nlohmann::json& payload, AgentOutputParseState* state,
                          std::vector<nlohmann::json>* out_bodies);

void DrainBufferedBodies(AgentOutputParseState* state, std::vector<nlohmann::json>* out_bodies);

}  // namespace ferryman::codeagent::parser
