#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ferryman::pty {

class PtyManager {
 public:
  using OutputCallback = std::function<void(const std::string& terminal_id, const std::string& data)>;

  PtyManager() = default;
  ~PtyManager();

  void SetOutputCallback(OutputCallback callback);
  std::optional<std::string> CreateTerminal(const std::string& owner_token, int cols, int rows,
                                            std::string* error);
  bool WriteInput(const std::string& owner_token, const std::string& terminal_id, const std::string& data,
                  std::string* error);
  bool Resize(const std::string& owner_token, const std::string& terminal_id, int cols, int rows,
              std::string* error);
  bool CloseTerminal(const std::string& owner_token, const std::string& terminal_id, std::string* error);
  std::vector<std::string> ListTerminals(const std::string& owner_token);
  void Shutdown();

 private:
  struct Terminal;

  std::shared_ptr<Terminal> FindTerminal(const std::string& owner_token, const std::string& terminal_id);
  void ReaderLoop(const std::shared_ptr<Terminal>& terminal);

  std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<Terminal>> terminals_;
  OutputCallback output_callback_;
};

}  // namespace ferryman::pty
