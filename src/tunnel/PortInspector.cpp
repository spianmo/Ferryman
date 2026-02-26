#include "ferryman/tunnel/PortInspector.hpp"

#include "ferryman/util/StringUtil.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ferryman::tunnel {

namespace {

std::string RunCommandCapture(const char* command) {
  if (command == nullptr || command[0] == '\0') {
    return "";
  }
#if defined(_WIN32)
  FILE* pipe = ::_popen(command, "r");
#else
  FILE* pipe = ::popen(command, "r");
#endif
  if (pipe == nullptr) {
    return "";
  }

  std::string output;
  std::array<char, 1024> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output.append(buffer.data());
  }

#if defined(_WIN32)
  (void)::_pclose(pipe);
#else
  (void)::pclose(pipe);
#endif
  return output;
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::pair<std::string, int> ParseEndpointAddressAndPort(const std::string& endpoint) {
  std::string trimmed = util::Trim(endpoint);
  if (trimmed.empty()) {
    return {"", 0};
  }

  std::string address;
  std::string port_text;
  if (trimmed.front() == '[') {
    const size_t close_bracket = trimmed.find(']');
    if (close_bracket != std::string::npos) {
      address = trimmed.substr(1, close_bracket - 1);
      if (close_bracket + 2 <= trimmed.size() && trimmed[close_bracket + 1] == ':') {
        port_text = trimmed.substr(close_bracket + 2);
      }
    }
  }

  if (port_text.empty()) {
    const size_t colon = trimmed.rfind(':');
    if (colon == std::string::npos) {
      return {"", 0};
    }
    address = trimmed.substr(0, colon);
    port_text = trimmed.substr(colon + 1);
  }

  if (address == "*") {
    address = "0.0.0.0";
  }
  if (address.empty()) {
    address = "0.0.0.0";
  }

  int port = 0;
  try {
    port = std::stoi(port_text);
  } catch (...) {
    port = 0;
  }
  if (port <= 0 || port > 65535) {
    return {"", 0};
  }
  return {address, port};
}

void SortAndDeduplicate(std::vector<ListeningPortInfo>* entries) {
  if (entries == nullptr) {
    return;
  }
  std::set<std::tuple<std::string, std::string, int, std::string, int>> unique;
  std::vector<ListeningPortInfo> deduped;
  deduped.reserve(entries->size());
  for (const auto& item : *entries) {
    const auto key = std::make_tuple(item.protocol, item.address, item.port, item.process, item.pid);
    if (unique.insert(key).second) {
      deduped.push_back(item);
    }
  }
  std::sort(deduped.begin(), deduped.end(), [](const ListeningPortInfo& lhs, const ListeningPortInfo& rhs) {
    if (lhs.port != rhs.port) {
      return lhs.port < rhs.port;
    }
    if (lhs.protocol != rhs.protocol) {
      return lhs.protocol < rhs.protocol;
    }
    if (lhs.address != rhs.address) {
      return lhs.address < rhs.address;
    }
    return lhs.pid < rhs.pid;
  });
  *entries = std::move(deduped);
}

#if defined(__linux__)
std::vector<ListeningPortInfo> ListLinuxListeningPorts() {
  std::vector<ListeningPortInfo> out;
  const std::string raw = RunCommandCapture("ss -H -ltnup 2>/dev/null");
  if (raw.empty()) {
    return out;
  }

  static const std::regex kProcessPattern(R"("([^"]+)",pid=([0-9]+))");

  std::istringstream lines(raw);
  for (std::string line; std::getline(lines, line);) {
    std::string proto;
    std::string state;
    std::string recv_q;
    std::string send_q;
    std::string local_endpoint;
    std::string peer_endpoint;
    std::istringstream row(line);
    if (!(row >> proto >> state >> recv_q >> send_q >> local_endpoint >> peer_endpoint)) {
      continue;
    }

    proto = ToLower(proto);
    if (proto.rfind("tcp", 0) == 0) {
      proto = "tcp";
    } else if (proto.rfind("udp", 0) == 0) {
      proto = "udp";
    } else {
      continue;
    }

    const auto [address, port] = ParseEndpointAddressAndPort(local_endpoint);
    if (port <= 0) {
      continue;
    }

    std::string rest;
    std::getline(row, rest);
    std::string process;
    int pid = 0;
    std::smatch match;
    if (std::regex_search(rest, match, kProcessPattern) && match.size() >= 3) {
      process = match[1].str();
      try {
        pid = std::stoi(match[2].str());
      } catch (...) {
        pid = 0;
      }
    }

    out.push_back({
        .protocol = proto,
        .address = address,
        .port = port,
        .process = process,
        .pid = pid,
    });
  }

  SortAndDeduplicate(&out);
  return out;
}
#endif

#if defined(__APPLE__)
std::vector<ListeningPortInfo> ListMacListeningPorts() {
  std::vector<ListeningPortInfo> out;
  const std::string raw = RunCommandCapture("lsof -nP -iTCP -sTCP:LISTEN -iUDP 2>/dev/null");
  if (raw.empty()) {
    return out;
  }

  static const std::regex kLinePattern(R"(^(\S+)\s+([0-9]+)\s+\S+.*\s+(TCP|UDP)\s+(\S+).*$)");
  std::istringstream lines(raw);
  for (std::string line; std::getline(lines, line);) {
    std::smatch match;
    if (!std::regex_match(line, match, kLinePattern) || match.size() < 5) {
      continue;
    }

    const std::string process = match[1].str();
    const std::string proto = ToLower(match[3].str());
    const std::string endpoint = match[4].str();

    int pid = 0;
    try {
      pid = std::stoi(match[2].str());
    } catch (...) {
      pid = 0;
    }
    const auto [address, port] = ParseEndpointAddressAndPort(endpoint);
    if (port <= 0) {
      continue;
    }

    out.push_back({
        .protocol = proto,
        .address = address,
        .port = port,
        .process = process,
        .pid = pid,
    });
  }

  SortAndDeduplicate(&out);
  return out;
}
#endif

#if defined(_WIN32)
std::vector<ListeningPortInfo> ListWindowsListeningPorts() {
  std::vector<ListeningPortInfo> out;
  const std::string raw = RunCommandCapture("cmd /c \"netstat -ano -p tcp && netstat -ano -p udp\"");
  if (raw.empty()) {
    return out;
  }

  std::istringstream lines(raw);
  for (std::string line; std::getline(lines, line);) {
    line = util::Trim(line);
    if (line.empty()) {
      continue;
    }
    if (line.rfind("TCP", 0) != 0 && line.rfind("UDP", 0) != 0) {
      continue;
    }

    std::istringstream row(line);
    std::string proto;
    std::string local_endpoint;
    std::string foreign_endpoint;
    std::string state;
    std::string pid_text;
    row >> proto >> local_endpoint >> foreign_endpoint;
    proto = ToLower(proto);
    if (proto == "tcp") {
      row >> state >> pid_text;
      if (ToLower(state) != "listening") {
        continue;
      }
    } else {
      row >> pid_text;
    }

    int pid = 0;
    try {
      pid = std::stoi(pid_text);
    } catch (...) {
      pid = 0;
    }
    const auto [address, port] = ParseEndpointAddressAndPort(local_endpoint);
    if (port <= 0) {
      continue;
    }

    out.push_back({
        .protocol = proto,
        .address = address,
        .port = port,
        .process = "",
        .pid = pid,
    });
  }

  SortAndDeduplicate(&out);
  return out;
}
#endif

}  // namespace

std::vector<ListeningPortInfo> PortInspector::ListListeningPorts() {
#if defined(__linux__)
  return ListLinuxListeningPorts();
#elif defined(__APPLE__)
  return ListMacListeningPorts();
#elif defined(_WIN32)
  return ListWindowsListeningPorts();
#else
  return {};
#endif
}

}  // namespace ferryman::tunnel
