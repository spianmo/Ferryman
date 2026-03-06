int ServerApp::HandleLogin(HttpRequest* req, HttpResponse* resp) {
  std::string provided = util::Trim(req->body);
  if (!provided.empty() && provided.front() == '{') {
    const auto payload = ParseJsonOrNull(provided);
    provided = JsonString(payload, "access_key",
                          JsonString(payload, "accessKey",
                                     JsonString(payload, "access_token",
                                                JsonString(payload, "accessToken",
                                                           JsonString(payload, "token", provided)))));
  }

  if (!auth_application_service_.ValidateAccessKey(provided, config_.access_key)) {
    return Json(resp, 401, api::Error("invalid access key", "unauthorized"));
  }

  std::string client_ip = req->client_addr.ip;
  if (client_ip.empty()) {
    client_ip = "unknown";
  }

  const std::string token = auth_application_service_.CreateSession(client_ip);

  return Json(resp, 200, api::Success({
                             {"session_token", token, false},
                             {"ws_port", std::to_string(config_.ws_port), true},
                             {"http_port", std::to_string(config_.http_port), true},
                             {"https_enabled", config_.https_enabled ? "true" : "false", true},
                             {"https_port", std::to_string(config_.https_port), true},
                             {"host", config_.http_host, false},
                         }));
}

int ServerApp::HandleSessionMe(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string host_os = HostOsTag();
  const bool docker_installed = DetectDockerInstalled();
  const bool codeserver_installed = DetectCodeServerInstalled();
  const bool kvm_installed = host_os == "linux" ? DetectKvmInstalled() : false;

  return Json(resp, 200,
              api::Success({
                  {"session_id", session->session_id, false},
                  {"client_ip", session->client_ip, false},
                  {"created_at", std::to_string(session->created_at), true},
                  {"last_seen_at", std::to_string(session->last_seen_at), true},
                  {"command_authorized", "true", true},
                  {"screen_authorized", "true", true},
                  {"host_os", host_os, false},
                  {"docker_installed", docker_installed ? "true" : "false", true},
                  {"codeserver_installed", codeserver_installed ? "true" : "false", true},
                  {"kvm_installed", kvm_installed ? "true" : "false", true},
              }));
}

int ServerApp::HandleFileList(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string request_path = QueryOf(req, "path");
  const auto resolved_path = file_application_service_.ResolvePath(request_path);
  if (!resolved_path.has_value()) {
    return Json(resp, 400, api::Error("path not allowed"));
  }

  std::string error;
  const auto entries = file_application_service_.ListDirectory(request_path, &error);
  if (!error.empty()) {
    return Json(resp, 400, api::Error(error));
  }

  const auto root_path = file_application_service_.ResolvePath("/");
  const std::string root_path_value =
      root_path.has_value() ? root_path->string() : config_.workspace_root.string();

  std::vector<std::string> serialized;
  serialized.reserve(entries.size());
  for (const auto& entry : entries) {
    serialized.push_back(util::BuildJsonObject({
        {"name", entry.name, false},
        {"path", entry.path, false},
        {"is_directory", entry.is_directory ? "true" : "false", true},
        {"size", std::to_string(entry.size), true},
        {"modified_at", std::to_string(entry.modified_at), true},
        {"permissions", entry.permissions, false},
    }));
  }

  return Json(resp, 200, api::Success({
                             {"entries", JsonArray(serialized), true},
                             {"current_path", resolved_path->string(), false},
                             {"root_path", root_path_value, false},
                         }));
}

int ServerApp::HandleFileRead(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string path = QueryOf(req, "path");
  std::string error;
  const auto content = file_application_service_.ReadFile(path, &error);
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
  if (!file_application_service_.WriteFile(path, content, &error)) {
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

  const std::string task_id = task_application_service_.StartTask(session->token, command);
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

  const auto tasks = task_application_service_.ListTasks(session->token);
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
  auto task = task_application_service_.GetTask(session->token, task_id);
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

int ServerApp::HandleDockurrList(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  std::string error;
  const auto vms = dockurr_application_service_.ListVms(&error);
  if (!error.empty()) {
    return Json(resp, 500, api::Error(error, "dockurr_unavailable"));
  }
  return Json(resp, 200, BuildDockurrSnapshotPayload(vms));
}

int ServerApp::HandleDockurrCreate(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  dockurr::CreateVmRequest create_request;
  create_request.os = ToLower(JsonString(payload, "os"));
  create_request.version = JsonString(payload, "version");
  create_request.ram_size = JsonString(payload, "ram", JsonString(payload, "ram_size", "4G"));
  create_request.disk_size = JsonString(payload, "disk", JsonString(payload, "disk_size", "64G"));
  create_request.name = JsonString(payload, "name");
  create_request.persistent = JsonBool(payload, "persist", JsonBool(payload, "persistent", false));

  if (create_request.os.empty()) {
    return Json(resp, 400, api::Error("os is required"));
  }

  dockurr::VmInfo vm;
  std::string error;
  if (!dockurr_application_service_.CreateVm(create_request, &vm, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos ||
                             lowered.find("required") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "dockurr_failed"));
  }

  audit_logger_.Append(session->token, "dockurr.create",
                       vm.name + " (" + create_request.os + " " + create_request.version + ")");
  return Json(resp, 200, api::Success({
                             {"vm", SerializeDockurrVm(vm), true},
                         }));
}

int ServerApp::HandleDockurrStart(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string name = JsonString(payload, "name", QueryOf(req, "name"));
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string error;
  if (!dockurr_application_service_.StartVm(name, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "dockurr_failed"));
  }

  audit_logger_.Append(session->token, "dockurr.start", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                         }));
}

int ServerApp::HandleDockurrStop(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string name = JsonString(payload, "name", QueryOf(req, "name"));
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string error;
  if (!dockurr_application_service_.StopVm(name, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "dockurr_failed"));
  }

  audit_logger_.Append(session->token, "dockurr.stop", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                         }));
}

int ServerApp::HandleDockurrRestart(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string name = JsonString(payload, "name", QueryOf(req, "name"));
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string error;
  if (!dockurr_application_service_.RestartVm(name, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "dockurr_failed"));
  }

  audit_logger_.Append(session->token, "dockurr.restart", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                         }));
}

int ServerApp::HandleDockurrDelete(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string name = JsonString(payload, "name", QueryOf(req, "name"));
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string error;
  if (!dockurr_application_service_.DeleteVm(name, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "dockurr_failed"));
  }

  audit_logger_.Append(session->token, "dockurr.delete", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                         }));
}

int ServerApp::HandleDockurrLogs(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string name = QueryOf(req, "name");
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }
  const int tail = ParseInt(QueryOf(req, "tail"), 50);

  std::string logs;
  std::string error;
  if (!dockurr_application_service_.GetLogs(name, tail, &logs, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "dockurr_failed"));
  }

  audit_logger_.Append(session->token, "dockurr.logs", name + " (tail=" + std::to_string(tail) + ")");
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"logs", logs, false},
                         }));
}

int ServerApp::HandleDockurrInspect(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string name = QueryOf(req, "name");
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string inspect;
  std::string error;
  if (!dockurr_application_service_.InspectVm(name, &inspect, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "dockurr_failed"));
  }

  audit_logger_.Append(session->token, "dockurr.inspect", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"inspect", inspect, false},
                         }));
}

int ServerApp::HandleDockerList(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const bool include_all = ParseBool(QueryOf(req, "all"), true);
  std::string error;
  const auto containers = docker_application_service_.ListContainers(include_all, &error);
  if (!error.empty()) {
    return Json(resp, 500, api::Error(error, DockerErrorLooksDaemonInactive(error) ? "docker_daemon_inactive" : "docker_unavailable"));
  }

  std::vector<std::string> serialized;
  serialized.reserve(containers.size());
  for (const auto& container : containers) {
    serialized.push_back(SerializeDockerContainer(container));
  }

  return Json(resp, 200, api::Success({
                             {"containers", JsonArray(serialized), true},
                             {"all", include_all ? "true" : "false", true},
                         }));
}

int ServerApp::HandleDockerServiceStart(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  std::string error;
  if (!docker_application_service_.StartDockerService(&error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("only supported") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_service_failed"));
  }

  audit_logger_.Append(session->token, "docker.service.start", "docker");
  return Json(resp, 200, api::Success({
                             {"started", "true", true},
                         }));
}

int ServerApp::HandleDockerStart(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string name = JsonString(payload, "name", QueryOf(req, "name"));
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string error;
  if (!docker_application_service_.StartContainer(name, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  audit_logger_.Append(session->token, "docker.start", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                         }));
}

int ServerApp::HandleDockerStop(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string name = JsonString(payload, "name", QueryOf(req, "name"));
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string error;
  if (!docker_application_service_.StopContainer(name, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  audit_logger_.Append(session->token, "docker.stop", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                         }));
}

int ServerApp::HandleDockerRestart(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string name = JsonString(payload, "name", QueryOf(req, "name"));
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string error;
  if (!docker_application_service_.RestartContainer(name, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  audit_logger_.Append(session->token, "docker.restart", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                         }));
}

int ServerApp::HandleDockerLogs(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string name = QueryOf(req, "name");
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }
  const int tail = ParseInt(QueryOf(req, "tail"), 160);

  std::string logs;
  std::string error;
  if (!docker_application_service_.GetLogs(name, tail, &logs, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  audit_logger_.Append(session->token, "docker.logs", name + " (tail=" + std::to_string(tail) + ")");
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"logs", logs, false},
                         }));
}

int ServerApp::HandleDockerInspect(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string name = QueryOf(req, "name");
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  std::string inspect;
  std::string error;
  if (!docker_application_service_.InspectContainer(name, &inspect, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  audit_logger_.Append(session->token, "docker.inspect", name);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"inspect", inspect, false},
                         }));
}

int ServerApp::HandleDockerStats(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string name = QueryOf(req, "name");
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }

  docker_runtime::ContainerStats stats;
  std::string error;
  if (!docker_application_service_.GetStats(name, &stats, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"stats", SerializeDockerStats(stats), true},
                         }));
}

int ServerApp::HandleDockerProcesses(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string name = QueryOf(req, "name");
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }
  const int limit = ParseInt(QueryOf(req, "limit"), 120);

  docker_runtime::ContainerProcessSnapshot processes;
  std::string error;
  if (!docker_application_service_.GetProcesses(name, limit, &processes, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  std::vector<std::string> serialized_rows;
  serialized_rows.reserve(processes.rows.size());
  for (const auto& row : processes.rows) {
    serialized_rows.push_back(SerializeStringArray(row));
  }

  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"columns", SerializeStringArray(processes.columns), true},
                             {"rows", JsonArray(serialized_rows), true},
                         }));
}

int ServerApp::HandleDockerFileList(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string name = QueryOf(req, "name");
  if (name.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }
  const std::string path = QueryOf(req, "path");

  std::vector<docker_runtime::ContainerFileEntry> entries;
  std::string current_path;
  std::string error;
  if (!docker_application_service_.ListFiles(name, path, &entries, &current_path, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos ||
                             lowered.find("directory") != std::string::npos ||
                             lowered.find("required") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  std::vector<std::string> serialized;
  serialized.reserve(entries.size());
  for (const auto& entry : entries) {
    serialized.push_back(SerializeDockerFileEntry(entry));
  }

  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"entries", JsonArray(serialized), true},
                             {"current_path", current_path, false},
                         }));
}

int ServerApp::HandleDockerFileRead(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const std::string name = QueryOf(req, "name");
  const std::string path = QueryOf(req, "path");
  if (name.empty() || path.empty()) {
    return Json(resp, 400, api::Error("name and path are required"));
  }

  std::string content;
  std::string error;
  if (!docker_application_service_.ReadFile(name, path, &content, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos ||
                             lowered.find("path") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  audit_logger_.Append(session->token, "docker.file.read", name + ":" + path);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"path", path, false},
                             {"content_base64", util::Base64Encode(content), false},
                         }));
}

int ServerApp::HandleDockerFileWrite(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string name = JsonString(payload, "name", QueryOf(req, "name"));
  const std::string path = JsonString(payload, "path", QueryOf(req, "path"));
  if (name.empty() || path.empty()) {
    return Json(resp, 400, api::Error("name and path are required"));
  }

  bool base64 = ParseBool(QueryOf(req, "base64"), true);
  std::string raw = req->body;
  if (payload.has_value()) {
    if (payload->contains("content_base64")) {
      raw = JsonString(payload, "content_base64");
      base64 = true;
    } else if (payload->contains("content")) {
      raw = JsonString(payload, "content");
      if (QueryOf(req, "base64").empty()) {
        base64 = false;
      }
    }
  }
  const std::string content = base64 ? util::Base64Decode(raw) : raw;

  std::string error;
  if (!docker_application_service_.WriteFile(name, path, content, &error)) {
    const std::string lowered = ToLower(error);
    const bool bad_request = lowered.find("invalid") != std::string::npos ||
                             lowered.find("path") != std::string::npos;
    return Json(resp, bad_request ? 400 : 500, api::Error(error, bad_request ? "bad_request" : "docker_failed"));
  }

  audit_logger_.Append(session->token, "docker.file.write", name + ":" + path);
  return Json(resp, 200, api::Success({
                             {"name", name, false},
                             {"path", path, false},
                             {"bytes", std::to_string(content.size()), true},
                         }));
}

int ServerApp::HandleScreenCaps(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  return Json(resp, 200, api::Success({
                             {"capabilities", screen_application_service_.CapabilitiesJson(), true},
                             {"screen_authorized", "true", true},
                             {"native_capture_running", screen_application_service_.IsCapturing() ? "true" : "false", true},
                         }));
}

int ServerApp::HandleScreenSources(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  std::string source_error;
  const auto sources = screen_application_service_.ListCaptureSources(&source_error);
  if (sources.empty()) {
    return Json(resp, 500, api::Error(source_error.empty() ? "no capture source available" : source_error));
  }

  json source_payload = json::array();
  std::string default_source_id;
  for (const auto& source : sources) {
    if (default_source_id.empty() && source.is_default) {
      default_source_id = source.id;
    }
    source_payload.push_back({
        {"id", source.id},
        {"name", source.name},
        {"width", source.width},
        {"height", source.height},
        {"is_default", source.is_default},
    });
  }
  if (default_source_id.empty()) {
    default_source_id = sources.front().id;
  }

  std::string active_source_id = screen_application_service_.ActiveCaptureSourceId();
  if (active_source_id.empty()) {
    active_source_id = screen_application_service_.NormalizeCaptureSourceId(default_source_id, nullptr);
  }
  if (active_source_id.empty()) {
    active_source_id = default_source_id;
  }

  return Json(resp, 200, api::Success({
                             {"sources", source_payload.dump(), true},
                             {"default_source_id", default_source_id, false},
                             {"active_source_id", active_source_id, false},
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
  if (!screen_application_service_.InjectInputEvent(session->token, {type, event_payload}, &error)) {
    return Json(resp, 403, api::Error(error, "forbidden"));
  }

  audit_logger_.Append(session->token, "screen.input", type);
  return Json(resp, 200, api::Success({
                             {"accepted", "true", true},
                             {"message", error, false},
                         }));
}

int ServerApp::HandleScreenUploadPreflight(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  if (!payload.has_value() || !payload->is_object()) {
    return Json(resp, 400, api::Error("invalid payload"));
  }
  const auto names_it = payload->find("names");
  if (names_it == payload->end() || !names_it->is_array()) {
    return Json(resp, 400, api::Error("names is required"));
  }

  const std::filesystem::path target_directory = ResolveDropTargetDirectory();
  std::error_code fs_error;
  if (!std::filesystem::exists(target_directory, fs_error)) {
    std::filesystem::create_directories(target_directory, fs_error);
  }
  if (fs_error || !std::filesystem::is_directory(target_directory)) {
    return Json(resp, 500, api::Error("failed to resolve upload target directory"));
  }

  json conflict_names = json::array();
  for (const auto& raw_name_value : *names_it) {
    std::string raw_name;
    if (raw_name_value.is_string()) {
      raw_name = raw_name_value.get<std::string>();
    } else {
      raw_name = raw_name_value.dump();
    }
    const std::string safe_name = SanitizeUploadFilename(raw_name);
    const std::filesystem::path target_path = target_directory / safe_name;
    const bool exists = std::filesystem::exists(target_path, fs_error);
    if (fs_error) {
      return Json(resp, 500, api::Error("failed to check target file state"));
    }
    if (exists) {
      conflict_names.push_back(safe_name);
    }
  }

  return Json(resp, 200, api::Success({
                             {"target_dir", target_directory.string(), false},
                             {"conflicts", conflict_names.dump(), true},
                             {"has_conflicts", conflict_names.empty() ? "false" : "true", true},
                             {"checked_count", std::to_string(names_it->size()), true},
                         }));
}

int ServerApp::HandleScreenUploadBegin(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  std::string transfer_id = util::Trim(JsonString(payload, "transfer_id"));
  if (transfer_id.empty()) {
    transfer_id = util::RandomHex(16);
  }
  const std::string file_name_raw = util::Trim(JsonString(payload, "name"));
  if (file_name_raw.empty()) {
    return Json(resp, 400, api::Error("name is required"));
  }
  std::string conflict_strategy = ToLower(util::Trim(JsonString(payload, "conflict_strategy", "keep_both")));
  if (conflict_strategy.empty()) {
    conflict_strategy = "keep_both";
  }
  if (conflict_strategy != "overwrite" && conflict_strategy != "keep_both" && conflict_strategy != "skip") {
    return Json(resp, 400, api::Error("invalid conflict_strategy"));
  }

  const std::filesystem::path target_directory = ResolveDropTargetDirectory();
  std::error_code fs_error;
  if (!std::filesystem::exists(target_directory, fs_error)) {
    std::filesystem::create_directories(target_directory, fs_error);
  }
  if (fs_error || !std::filesystem::is_directory(target_directory)) {
    return Json(resp, 500, api::Error("failed to resolve upload target directory"));
  }

  const std::string requested_safe_name = SanitizeUploadFilename(file_name_raw);
  const std::filesystem::path desired_path = target_directory / requested_safe_name;
  const bool desired_exists = std::filesystem::exists(desired_path, fs_error);
  if (fs_error) {
    return Json(resp, 500, api::Error("failed to check target file state"));
  }
  if (desired_exists && std::filesystem::is_directory(desired_path, fs_error)) {
    return Json(resp, 400, api::Error("target path is a directory"));
  }
  if (conflict_strategy == "skip" && desired_exists) {
    return Json(resp, 200, api::Success({
                               {"transfer_id", transfer_id, false},
                               {"target_dir", target_directory.string(), false},
                               {"path", desired_path.string(), false},
                               {"requested_name", requested_safe_name, false},
                               {"saved_name", requested_safe_name, false},
                               {"name_conflict", "true", true},
                               {"skip_existing", "true", true},
                               {"accepted", "true", true},
                           }));
  }

  std::filesystem::path temp_root = std::filesystem::temp_directory_path(fs_error) / "ferryman-screen-upload";
  if (fs_error) {
    temp_root = std::filesystem::current_path() / ".ferryman-screen-upload";
    fs_error.clear();
  }
  std::filesystem::create_directories(temp_root, fs_error);
  if (fs_error) {
    return Json(resp, 500, api::Error("failed to prepare temporary upload directory"));
  }

  ScreenUploadTransfer transfer;
  transfer.owner_session_token = session->token;
  transfer.transfer_id = transfer_id;
  transfer.file_name = file_name_raw;
  transfer.conflict_strategy = conflict_strategy;
  transfer.target_directory = target_directory;
  transfer.expected_bytes = JsonUint64(payload, "size", 0);
  transfer.temp_path = temp_root / (util::RandomHex(16) + ".part");
  transfer.stream.open(transfer.temp_path, std::ios::binary | std::ios::trunc);
  if (!transfer.stream.is_open()) {
    return Json(resp, 500, api::Error("failed to open temporary upload file"));
  }

  const std::string map_key = session->token + ":" + transfer_id;
  {
    std::lock_guard<std::mutex> lock(screen_upload_mu_);
    if (screen_uploads_.find(map_key) != screen_uploads_.end()) {
      transfer.stream.close();
      std::filesystem::remove(transfer.temp_path, fs_error);
      return Json(resp, 409, api::Error("transfer already exists", "conflict"));
    }
    screen_uploads_.emplace(map_key, std::move(transfer));
  }

  return Json(resp, 200, api::Success({
                             {"transfer_id", transfer_id, false},
                             {"target_dir", target_directory.string(), false},
                             {"path", desired_path.string(), false},
                             {"requested_name", requested_safe_name, false},
                             {"saved_name", requested_safe_name, false},
                             {"name_conflict", desired_exists ? "true" : "false", true},
                             {"skip_existing", "false", true},
                             {"accepted", "true", true},
                         }));
}

int ServerApp::HandleScreenUploadChunk(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string transfer_id = util::Trim(JsonString(payload, "transfer_id"));
  if (transfer_id.empty()) {
    return Json(resp, 400, api::Error("transfer_id is required"));
  }
  const std::string data_base64 = JsonString(payload, "data_base64");
  if (data_base64.empty()) {
    return Json(resp, 400, api::Error("data_base64 is required"));
  }

  const std::string chunk = util::Base64Decode(data_base64);
  if (chunk.empty() && !data_base64.empty()) {
    // Allow empty chunk payloads that decode to empty bytes.
  }

  const std::string map_key = session->token + ":" + transfer_id;
  std::uint64_t received_bytes = 0;
  std::uint64_t expected_bytes = 0;
  std::filesystem::path temp_path;
  bool overrun = false;

  {
    std::lock_guard<std::mutex> lock(screen_upload_mu_);
    auto it = screen_uploads_.find(map_key);
    if (it == screen_uploads_.end()) {
      return Json(resp, 404, api::Error("transfer not found", "not_found"));
    }
    ScreenUploadTransfer& transfer = it->second;
    if (transfer.owner_session_token != session->token) {
      return Json(resp, 403, api::Error("forbidden", "forbidden"));
    }
    transfer.stream.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    if (!transfer.stream.good()) {
      temp_path = transfer.temp_path;
      transfer.stream.close();
      screen_uploads_.erase(it);
      std::error_code remove_error;
      std::filesystem::remove(temp_path, remove_error);
      return Json(resp, 500, api::Error("failed to append upload chunk"));
    }
    transfer.received_bytes += static_cast<std::uint64_t>(chunk.size());
    received_bytes = transfer.received_bytes;
    expected_bytes = transfer.expected_bytes;
    temp_path = transfer.temp_path;
    if (expected_bytes > 0 && received_bytes > expected_bytes) {
      transfer.stream.close();
      screen_uploads_.erase(it);
      overrun = true;
    }
  }

  if (overrun) {
    std::error_code remove_error;
    std::filesystem::remove(temp_path, remove_error);
    return Json(resp, 400, api::Error("received bytes exceed declared upload size"));
  }

  return Json(resp, 200, api::Success({
                             {"transfer_id", transfer_id, false},
                             {"received_bytes", std::to_string(received_bytes), true},
                             {"expected_bytes", std::to_string(expected_bytes), true},
                         }));
}

int ServerApp::HandleScreenUploadCommit(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string transfer_id = util::Trim(JsonString(payload, "transfer_id"));
  if (transfer_id.empty()) {
    return Json(resp, 400, api::Error("transfer_id is required"));
  }

  const std::string map_key = session->token + ":" + transfer_id;
  ScreenUploadTransfer transfer;
  {
    std::lock_guard<std::mutex> lock(screen_upload_mu_);
    auto it = screen_uploads_.find(map_key);
    if (it == screen_uploads_.end()) {
      return Json(resp, 404, api::Error("transfer not found", "not_found"));
    }
    transfer = std::move(it->second);
    screen_uploads_.erase(it);
  }

  transfer.stream.flush();
  transfer.stream.close();

  if (transfer.expected_bytes > 0 && transfer.received_bytes != transfer.expected_bytes) {
    std::error_code remove_error;
    std::filesystem::remove(transfer.temp_path, remove_error);
    return Json(resp, 400, api::Error("upload is incomplete"));
  }

  std::error_code fs_error;
  if (!std::filesystem::exists(transfer.target_directory, fs_error)) {
    std::filesystem::create_directories(transfer.target_directory, fs_error);
  }
  if (fs_error || !std::filesystem::is_directory(transfer.target_directory)) {
    std::filesystem::remove(transfer.temp_path, fs_error);
    return Json(resp, 500, api::Error("target directory is unavailable"));
  }

  std::string conflict_strategy = transfer.conflict_strategy;
  if (conflict_strategy.empty()) {
    conflict_strategy = "keep_both";
  }
  const std::string requested_safe_name = SanitizeUploadFilename(transfer.file_name);
  const std::filesystem::path desired_path = transfer.target_directory / requested_safe_name;
  const bool desired_exists = std::filesystem::exists(desired_path, fs_error);
  if (fs_error) {
    std::filesystem::remove(transfer.temp_path, fs_error);
    return Json(resp, 500, api::Error("failed to check target file state"));
  }
  if (desired_exists && std::filesystem::is_directory(desired_path, fs_error)) {
    std::filesystem::remove(transfer.temp_path, fs_error);
    return Json(resp, 400, api::Error("target path is a directory"));
  }

  bool skipped = false;
  bool overwritten = false;
  bool name_conflict = false;
  std::filesystem::path final_path;
  if (conflict_strategy == "overwrite") {
    final_path = desired_path;
    name_conflict = desired_exists;
    overwritten = desired_exists;
    if (desired_exists) {
      std::filesystem::remove(desired_path, fs_error);
      if (fs_error) {
        std::filesystem::remove(transfer.temp_path, fs_error);
        return Json(resp, 500, api::Error("failed to remove existing file for overwrite"));
      }
    }
  } else if (conflict_strategy == "skip" && desired_exists) {
    final_path = desired_path;
    skipped = true;
    name_conflict = true;
  } else {
    final_path = ResolveUniqueTargetPath(transfer.target_directory, transfer.file_name);
    name_conflict = desired_path != final_path;
  }

  if (skipped) {
    std::filesystem::remove(transfer.temp_path, fs_error);
    audit_logger_.Append(session->token, "screen.upload.skip", final_path.string());
    return Json(resp, 200, api::Success({
                               {"transfer_id", transfer_id, false},
                               {"name", requested_safe_name, false},
                               {"requested_name", requested_safe_name, false},
                               {"saved_name", requested_safe_name, false},
                               {"name_conflict", "true", true},
                               {"skipped", "true", true},
                               {"overwritten", "false", true},
                               {"path", final_path.string(), false},
                               {"target_dir", transfer.target_directory.string(), false},
                               {"bytes", "0", true},
                           }));
  }

  std::filesystem::rename(transfer.temp_path, final_path, fs_error);
  if (fs_error) {
    fs_error.clear();
    const auto copy_mode = conflict_strategy == "overwrite"
                               ? std::filesystem::copy_options::overwrite_existing
                               : std::filesystem::copy_options::none;
    std::filesystem::copy_file(transfer.temp_path, final_path, copy_mode, fs_error);
    if (fs_error) {
      std::filesystem::remove(transfer.temp_path, fs_error);
      return Json(resp, 500, api::Error("failed to finalize uploaded file"));
    }
    std::filesystem::remove(transfer.temp_path, fs_error);
  }

  audit_logger_.Append(session->token, "screen.upload", final_path.string());
  return Json(resp, 200, api::Success({
                             {"transfer_id", transfer_id, false},
                             {"name", SanitizeUploadFilename(transfer.file_name), false},
                             {"requested_name", requested_safe_name, false},
                             {"saved_name", final_path.filename().string(), false},
                             {"name_conflict", name_conflict ? "true" : "false", true},
                             {"skipped", "false", true},
                             {"overwritten", overwritten ? "true" : "false", true},
                             {"path", final_path.string(), false},
                             {"target_dir", transfer.target_directory.string(), false},
                             {"bytes", std::to_string(transfer.received_bytes), true},
                         }));
}

int ServerApp::HandleScreenUploadCancel(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string transfer_id = util::Trim(JsonString(payload, "transfer_id"));
  if (transfer_id.empty()) {
    return Json(resp, 400, api::Error("transfer_id is required"));
  }

  const std::string map_key = session->token + ":" + transfer_id;
  std::filesystem::path temp_path;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(screen_upload_mu_);
    auto it = screen_uploads_.find(map_key);
    if (it != screen_uploads_.end()) {
      found = true;
      temp_path = it->second.temp_path;
      it->second.stream.close();
      screen_uploads_.erase(it);
    }
  }
  if (found) {
    std::error_code remove_error;
    std::filesystem::remove(temp_path, remove_error);
  }

  return Json(resp, 200, api::Success({
                             {"transfer_id", transfer_id, false},
                             {"cancelled", found ? "true" : "false", true},
                         }));
}

int ServerApp::HandleCodeServerConfigUpdate(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }
  if (!DetectCodeServerInstalled()) {
    return Json(resp, 400, api::Error("code-server is not installed", "codeserver_not_installed"));
  }

  const auto payload = ParseJsonOrNull(req->body);
  const auto payload_has = [&payload](const char* key) -> bool {
    return payload.has_value() && payload->is_object() && payload->contains(key);
  };

  const std::string port_query = QueryOf(req, "port");
  const std::string alt_port_query = QueryOf(req, "codeserver_port");
  const int requested_port = JsonInt(payload, "port",
                                     JsonInt(payload, "codeserver_port",
                                             ParseInt(port_query.empty() ? alt_port_query : port_query, 0)));

  const std::string https_query = QueryOf(req, "https_enabled");
  const bool has_requested_https_enabled = payload_has("https_enabled") || !https_query.empty();
  const bool requested_https_enabled = JsonBool(payload, "https_enabled", ParseBool(https_query, true));

  const std::string tls_mode_query = QueryOf(req, "tls_mode");
  const std::string alt_tls_mode_query = QueryOf(req, "codeserver_https_mode");
  const bool has_requested_tls_mode =
      payload_has("tls_mode") || payload_has("codeserver_https_mode") || !tls_mode_query.empty() ||
      !alt_tls_mode_query.empty();
  const std::string requested_tls_mode = JsonString(
      payload, "tls_mode",
      JsonString(payload, "codeserver_https_mode", tls_mode_query.empty() ? alt_tls_mode_query : tls_mode_query));

  const std::string cert_query = QueryOf(req, "custom_cert_path");
  const std::string alt_cert_query = QueryOf(req, "codeserver_https_cert_file");
  const bool has_requested_cert_path =
      payload_has("custom_cert_path") || payload_has("codeserver_https_cert_file") || !cert_query.empty() ||
      !alt_cert_query.empty();
  const std::string requested_cert_path = util::Trim(
      JsonString(payload, "custom_cert_path",
                 JsonString(payload, "codeserver_https_cert_file", cert_query.empty() ? alt_cert_query : cert_query)));

  const std::string key_query = QueryOf(req, "custom_key_path");
  const std::string alt_key_query = QueryOf(req, "codeserver_https_key_file");
  const bool has_requested_key_path =
      payload_has("custom_key_path") || payload_has("codeserver_https_key_file") || !key_query.empty() ||
      !alt_key_query.empty();
  const std::string requested_key_path = util::Trim(
      JsonString(payload, "custom_key_path",
                 JsonString(payload, "codeserver_https_key_file", key_query.empty() ? alt_key_query : key_query)));

  core::AppConfig current_config;
  {
    std::lock_guard<std::mutex> lock(codeserver_mu_);
    current_config = config_;
  }

  const int current_port = LoadCodeServerPort(current_config);
  const bool current_https_enabled = LoadCodeServerHttpsEnabled(current_config);
  const std::string current_tls_mode = CodeServerTlsModeToString(LoadCodeServerTlsMode(current_config));
  const std::filesystem::path current_cert_path = LoadCodeServerCustomCertPath(current_config);
  const std::filesystem::path current_key_path = LoadCodeServerCustomKeyPath(current_config);

  const int next_port = requested_port > 0 ? requested_port : current_port;
  if (next_port <= 0 || next_port > 65535) {
    return Json(resp, 400, api::Error("codeserver port is invalid"));
  }

  const bool next_https_enabled = has_requested_https_enabled ? requested_https_enabled : current_https_enabled;
  const std::string next_tls_mode =
      has_requested_tls_mode ? NormalizeCodeServerTlsMode(requested_tls_mode, current_tls_mode) : current_tls_mode;
  const std::filesystem::path next_cert_path =
      has_requested_cert_path ? ResolveCodeServerStoredPath(requested_cert_path) : current_cert_path;
  const std::filesystem::path next_key_path =
      has_requested_key_path ? ResolveCodeServerStoredPath(requested_key_path) : current_key_path;

  if (next_https_enabled && next_tls_mode == "custom") {
    if (next_cert_path.empty() || next_key_path.empty()) {
      return Json(resp, 400, api::Error("custom cert mode requires cert and key paths"));
    }
    if (!FileExistsAndNonEmpty(next_cert_path) || !FileExistsAndNonEmpty(next_key_path)) {
      return Json(resp, 400, api::Error("custom cert/key not found or empty", "invalid_custom_cert_paths"));
    }
  }

  core::AppConfig runtime_config = current_config;
  runtime_config.codeserver_port = next_port;
  runtime_config.has_codeserver_port = true;
  runtime_config.codeserver_https_enabled = next_https_enabled;
  runtime_config.has_codeserver_https_enabled = true;
  runtime_config.codeserver_https_mode = next_tls_mode;
  runtime_config.has_codeserver_https_mode = true;
  runtime_config.codeserver_https_cert_file = next_cert_path;
  runtime_config.has_codeserver_https_cert_file = true;
  runtime_config.codeserver_https_key_file = next_key_path;
  runtime_config.has_codeserver_https_key_file = true;

  std::string persist_error;
  if (!PersistCodeServerConfigToDisk(runtime_config, next_port, next_https_enabled, next_tls_mode, next_cert_path,
                                     next_key_path, &persist_error)) {
    return Json(resp, 500,
                api::Error(persist_error.empty() ? "failed to persist code-server config" : persist_error));
  }

  {
    std::lock_guard<std::mutex> lock(codeserver_mu_);
    config_.codeserver_port = runtime_config.codeserver_port;
    config_.has_codeserver_port = runtime_config.has_codeserver_port;
    config_.codeserver_https_enabled = runtime_config.codeserver_https_enabled;
    config_.has_codeserver_https_enabled = runtime_config.has_codeserver_https_enabled;
    config_.codeserver_https_mode = runtime_config.codeserver_https_mode;
    config_.has_codeserver_https_mode = runtime_config.has_codeserver_https_mode;
    config_.codeserver_https_cert_file = runtime_config.codeserver_https_cert_file;
    config_.has_codeserver_https_cert_file = runtime_config.has_codeserver_https_cert_file;
    config_.codeserver_https_key_file = runtime_config.codeserver_https_key_file;
    config_.has_codeserver_https_key_file = runtime_config.has_codeserver_https_key_file;
  }

  std::string startup_detail;
  const bool ensured = EnsureCodeServerRunning(runtime_config, next_port, &startup_detail, true);
  audit_logger_.Append(session->token, ensured ? "codeserver.config.update" : "codeserver.config.update.failed",
                       startup_detail);
  if (!ensured) {
    return Json(resp, 500,
                api::Error("failed to restart code-server: " + startup_detail, "codeserver_restart_failed"));
  }

  return Json(resp, 200, api::Success({
                             {"codeserver_port", std::to_string(next_port), true},
                             {"https_enabled", next_https_enabled ? "true" : "false", true},
                             {"tls_mode", next_tls_mode, false},
                             {"custom_cert_path", next_cert_path.string(), false},
                             {"custom_key_path", next_key_path.string(), false},
                             {"scheme", next_https_enabled ? "https" : "http", false},
                             {"detail", startup_detail, false},
                         }));
}

int ServerApp::HandleTunnelState(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  std::string proxy_host;
  int proxy_port = 0;
  std::string proxy_token;
  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    proxy_host = config_.tunnel_proxy_host;
    proxy_port = config_.tunnel_proxy_port;
    proxy_token = config_.tunnel_proxy_token;
  }

  const auto snapshots = tunnel_application_service_.Snapshot();
  std::vector<std::string> serialized;
  serialized.reserve(snapshots.size());
  for (const auto& snapshot : snapshots) {
    serialized.push_back(SerializeTunnelSnapshot(snapshot));
  }

  return Json(resp, 200, api::Success({
                             {"proxy_host", proxy_host, false},
                             {"proxy_port", std::to_string(proxy_port), true},
                             {"proxy_token", proxy_token, false},
                             {"mappings", JsonArray(serialized), true},
                         }));
}

int ServerApp::HandleTunnelConfigUpdate(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string requested_host = util::Trim(JsonString(payload, "proxy_host", QueryOf(req, "proxy_host")));
  const int requested_port = JsonInt(payload, "proxy_port", ParseInt(QueryOf(req, "proxy_port"), 0));
  const std::string token_query = QueryOf(req, "proxy_token");
  const bool has_requested_token =
      (payload.has_value() && payload->is_object() && payload->contains("proxy_token")) || !token_query.empty();
  const std::string requested_token = util::Trim(JsonString(payload, "proxy_token", token_query));

  std::string previous_host;
  int previous_port = 0;
  std::string previous_token;
  std::vector<core::TunnelMappingConfig> mappings;
  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    previous_host = config_.tunnel_proxy_host;
    previous_port = config_.tunnel_proxy_port;
    previous_token = config_.tunnel_proxy_token;
    mappings = config_.tunnel_mappings;
  }

  const std::string next_host = requested_host;
  const int next_port = requested_port > 0 ? requested_port : previous_port;
  const std::string next_token = has_requested_token ? requested_token : previous_token;
  if (!next_host.empty() && (next_port <= 0 || next_port > 65535)) {
    return Json(resp, 400, api::Error("proxy_port is invalid"));
  }
  if (!next_host.empty()) {
    std::string probe_detail;
    if (!tunnel_application_service_.ProbeProxyEndpoint(next_host, next_port, next_token, &probe_detail)) {
      const std::string endpoint = next_host + ":" + std::to_string(next_port);
      const std::string message = "failed to connect FerrymanProxy at " + endpoint +
                                  (probe_detail.empty() ? std::string() : " (" + probe_detail + ")");
      return Json(resp, 400, api::Error(message, "proxy_unreachable"));
    }
  }

  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    config_.tunnel_proxy_host = next_host;
    config_.tunnel_proxy_port = next_port;
    config_.tunnel_proxy_token = next_token;
  }

  std::string persist_error;
  if (!PersistTunnelConfigToDisk(config_, next_host, next_port, next_token, mappings, &persist_error)) {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    config_.tunnel_proxy_host = previous_host;
    config_.tunnel_proxy_port = previous_port;
    config_.tunnel_proxy_token = previous_token;
    return Json(resp, 500, api::Error(persist_error.empty() ? "failed to persist tunnel config" : persist_error));
  }

  tunnel_application_service_.Configure(next_host, next_port, next_token, mappings);
  audit_logger_.Append(session->token, "tunnel.config.update", next_host + ":" + std::to_string(next_port));

  return Json(resp, 200, api::Success({
                             {"proxy_host", next_host, false},
                             {"proxy_port", std::to_string(next_port), true},
                             {"proxy_token", next_token, false},
                         }));
}

int ServerApp::HandleTunnelMappingUpsert(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const auto parsed_mapping = ParseTunnelMappingFromJson(payload);
  if (!parsed_mapping.has_value()) {
    return Json(resp, 400, api::Error("invalid mapping payload"));
  }
  core::TunnelMappingConfig mapping = *parsed_mapping;

  std::vector<core::TunnelMappingConfig> previous_mappings;
  std::vector<core::TunnelMappingConfig> next_mappings;
  std::string proxy_host;
  int proxy_port = 0;
  std::string proxy_token;
  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    previous_mappings = config_.tunnel_mappings;
    next_mappings = config_.tunnel_mappings;
    proxy_host = config_.tunnel_proxy_host;
    proxy_port = config_.tunnel_proxy_port;
    proxy_token = config_.tunnel_proxy_token;
  }

  const std::string mapping_protocol = NormalizeTunnelProtocol(mapping.protocol);
  bool had_previous = false;
  core::TunnelMappingConfig previous_mapping;
  for (const auto& existing : previous_mappings) {
    if (existing.id == mapping.id) {
      previous_mapping = existing;
      had_previous = true;
      break;
    }
  }

  bool replaced = false;
  for (auto& existing : next_mappings) {
    if (existing.id == mapping.id) {
      existing = mapping;
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    next_mappings.push_back(mapping);
  }

  if (mapping.enabled) {
    for (const auto& item : next_mappings) {
      if (item.id == mapping.id || !item.enabled) {
        continue;
      }
      if (NormalizeTunnelProtocol(item.protocol) == mapping_protocol && item.remote_port == mapping.remote_port) {
        return Json(resp, 409, api::Error("remote port conflict with another mapping", "conflict"));
      }
    }
  }

  const bool keeps_existing_bound_remote_port =
      had_previous && previous_mapping.enabled && mapping.enabled &&
      NormalizeTunnelProtocol(previous_mapping.protocol) == mapping_protocol &&
      previous_mapping.remote_port == mapping.remote_port;
  if (mapping.enabled && !keeps_existing_bound_remote_port && TunnelProxyHostLooksLocal(proxy_host)) {
    const auto listening_ports = tunnel::PortInspector::ListListeningPorts();
    const auto occupied = FindListeningPortConflict(listening_ports, mapping_protocol, mapping.remote_port);
    if (occupied.has_value()) {
      std::ostringstream message;
      message << "remote (public) port " << mapping.remote_port << "/" << mapping_protocol << " is already occupied";
      const std::string address = util::Trim(occupied->address).empty() ? "0.0.0.0" : occupied->address;
      message << " at " << address;
      if (occupied->pid > 0 || !occupied->process.empty()) {
        message << " by ";
        if (occupied->pid > 0) {
          message << "pid " << occupied->pid;
          if (!occupied->process.empty()) {
            message << ' ';
          }
        }
        if (!occupied->process.empty()) {
          message << occupied->process;
        }
      }
      return Json(resp, 409, api::Error(message.str(), "port_occupied"));
    }
  }

  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    config_.tunnel_mappings = next_mappings;
  }

  std::string persist_error;
  if (!PersistTunnelConfigToDisk(config_, proxy_host, proxy_port, proxy_token, next_mappings, &persist_error)) {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    config_.tunnel_mappings = previous_mappings;
    return Json(resp, 500, api::Error(persist_error.empty() ? "failed to persist mapping" : persist_error));
  }

  tunnel_application_service_.Configure(proxy_host, proxy_port, proxy_token, next_mappings);
  audit_logger_.Append(session->token, "tunnel.mapping.upsert",
                       mapping.id + " " + NormalizeTunnelProtocol(mapping.protocol) + " " +
                           std::to_string(mapping.local_port) + "->" + std::to_string(mapping.remote_port));

  return Json(resp, 200, api::Success({
                             {"mapping", SerializeTunnelMappingConfig(mapping), true},
                             {"updated", replaced ? "true" : "false", true},
                         }));
}

int ServerApp::HandleTunnelMappingDelete(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string mapping_id = util::Trim(JsonString(payload, "id", QueryOf(req, "id")));
  if (mapping_id.empty()) {
    return Json(resp, 400, api::Error("id is required"));
  }

  std::vector<core::TunnelMappingConfig> previous_mappings;
  std::vector<core::TunnelMappingConfig> next_mappings;
  std::string proxy_host;
  int proxy_port = 0;
  std::string proxy_token;
  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    previous_mappings = config_.tunnel_mappings;
    next_mappings = config_.tunnel_mappings;
    proxy_host = config_.tunnel_proxy_host;
    proxy_port = config_.tunnel_proxy_port;
    proxy_token = config_.tunnel_proxy_token;
  }

  const auto before_count = next_mappings.size();
  next_mappings.erase(std::remove_if(next_mappings.begin(), next_mappings.end(), [&](const core::TunnelMappingConfig& item) {
                      return item.id == mapping_id;
                    }),
                    next_mappings.end());
  if (next_mappings.size() == before_count) {
    return Json(resp, 404, api::Error("mapping not found", "not_found"));
  }

  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    config_.tunnel_mappings = next_mappings;
  }

  std::string persist_error;
  if (!PersistTunnelConfigToDisk(config_, proxy_host, proxy_port, proxy_token, next_mappings, &persist_error)) {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    config_.tunnel_mappings = previous_mappings;
    return Json(resp, 500, api::Error(persist_error.empty() ? "failed to persist mapping" : persist_error));
  }

  tunnel_application_service_.Configure(proxy_host, proxy_port, proxy_token, next_mappings);
  audit_logger_.Append(session->token, "tunnel.mapping.delete", mapping_id);

  return Json(resp, 200, api::Success({
                             {"id", mapping_id, false},
                         }));
}

int ServerApp::HandleTunnelMappingTest(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string mapping_id = util::Trim(JsonString(payload, "id", QueryOf(req, "id")));
  if (mapping_id.empty()) {
    return Json(resp, 400, api::Error("id is required"));
  }

  bool exists = false;
  {
    std::lock_guard<std::mutex> lock(tunnel_mu_);
    for (const auto& item : config_.tunnel_mappings) {
      if (item.id == mapping_id) {
        exists = true;
        break;
      }
    }
  }
  if (!exists) {
    return Json(resp, 404, api::Error("mapping not found", "not_found"));
  }

  bool test_ok = false;
  std::string detail;
  const bool executed = tunnel_application_service_.TestMapping(mapping_id, &test_ok, &detail, 5000);
  if (!executed && detail.empty()) {
    return Json(resp, 500, api::Error("failed to execute mapping test"));
  }

  audit_logger_.Append(session->token, "tunnel.mapping.test",
                       mapping_id + " result=" + std::string(test_ok ? "ok" : "failed") + " detail=" + detail);

  return Json(resp, 200, api::Success({
                             {"id", mapping_id, false},
                             {"test_ok", test_ok ? "true" : "false", true},
                             {"detail", detail, false},
                         }));
}

int ServerApp::HandleTunnelPorts(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }

  static std::mutex ports_cache_mu;
  static std::vector<tunnel::ListeningPortInfo> ports_cache;
  static auto ports_cache_at = std::chrono::steady_clock::time_point{};
  constexpr auto kPortsCacheTtl = std::chrono::milliseconds(1800);

  std::vector<tunnel::ListeningPortInfo> ports;
  bool cache_hit = false;
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> cache_lock(ports_cache_mu);
    if (ports_cache_at != std::chrono::steady_clock::time_point{} && (now - ports_cache_at) < kPortsCacheTtl) {
      ports = ports_cache;
      cache_hit = true;
    }
  }
  if (!cache_hit) {
    ports = tunnel::PortInspector::ListListeningPorts();
    std::lock_guard<std::mutex> cache_lock(ports_cache_mu);
    ports_cache = ports;
    ports_cache_at = std::chrono::steady_clock::now();
  }

  std::vector<std::string> serialized;
  serialized.reserve(ports.size());
  for (const auto& item : ports) {
    serialized.push_back(util::BuildJsonObject({
        {"protocol", item.protocol, false},
        {"address", item.address, false},
        {"port", std::to_string(item.port), true},
        {"process", item.process, false},
        {"pid", std::to_string(item.pid), true},
    }));
  }

  return Json(resp, 200, api::Success({
                             {"items", JsonArray(serialized), true},
                             {"count", std::to_string(serialized.size()), true},
                         }));
}

int ServerApp::HandleHealth(HttpRequest* req, HttpResponse* resp) {
  (void)req;
  return Json(resp, 200, api::Success({
                             {"service", "ferryman", false},
                             {"running", running_ ? "true" : "false", true},
                             {"http_port", std::to_string(config_.http_port), true},
                             {"https_enabled", config_.https_enabled ? "true" : "false", true},
                             {"https_port", std::to_string(config_.https_port), true},
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
