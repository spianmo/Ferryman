#pragma once

#include <string>
#include <vector>

namespace ferryman::tunnel {

struct ListeningPortInfo {
  std::string protocol;
  std::string address;
  int port = 0;
  std::string process;
  int pid = 0;
};

class PortInspector {
 public:
  static std::vector<ListeningPortInfo> ListListeningPorts();
};

}  // namespace ferryman::tunnel
