int ServerApp::HandleCodeAgentRunnerState(HttpRequest* req, HttpResponse* resp) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return resp->status_code;
  }
  const json state = codeagent_application_service_.BuildRunnerState("default");
  return Json(resp, 200, json{{"ok", true}, {"state", state}}.dump());
}

int ServerApp::HandleCodeAgentBind(HttpRequest* req, HttpResponse* resp) {
  (void)req;
  return Json(resp, 503, json{{"error", "Binding is not supported in Ferryman CodeAgent"}}.dump());
}

int ServerApp::HandleCodeAgentEvents(HttpRequest* req, HttpResponse* resp) {
  std::string bearer_token;
  auto ns = RequireCodeAgentNamespace(req, resp, &bearer_token);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  codeagent::EventQuery query;
  query.all = ParseBool(QueryOf(req, "all"), false);
  query.session_id = util::Trim(QueryOf(req, "sessionId"));
  query.machine_id = util::Trim(QueryOf(req, "machineId"));
  const std::string visibility = util::Trim(QueryOf(req, "visibility"));
  const int wait_ms = std::clamp(ParseInt(QueryOf(req, "waitMs"), 15000), 0, 30000);

  json drained = codeagent_application_service_.DrainEventsForToken(bearer_token, *ns, query, visibility, wait_ms);
  const std::string subscription_id = drained.value("subscriptionId", "sub-" + util::RandomHex(12));
  json events = drained.value("events", json::array());

  std::ostringstream stream;
  stream << "retry: 1000\n";
  stream << "data: "
         << json{
                {"type", "connection-changed"},
                {"data", {{"status", "connected"}, {"subscriptionId", subscription_id}}},
            }
                .dump()
         << "\n\n";

  if (!events.is_array() || events.empty()) {
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    stream << "data: " << json{{"type", "heartbeat"}, {"data", {{"timestamp", now_ms}}}}.dump() << "\n\n";
  } else {
    for (const auto& event : events) {
      stream << "data: " << event.dump() << "\n\n";
    }
  }

  resp->headers["Cache-Control"] = "no-cache";
  resp->headers["Connection"] = "keep-alive";
  return Text(resp, 200, stream.str(), "text/event-stream; charset=utf-8");
}

int ServerApp::HandleCodeAgentVisibility(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }
  return Json(resp, 200, json{{"ok", true}}.dump());
}

int ServerApp::HandleCodeAgentSessions(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }
  return Json(resp, 200, codeagent_application_service_.BuildSessionsResponse(*ns).dump());
}

int ServerApp::HandleCodeAgentSession(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || !tail.empty()) {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }

  const auto payload = codeagent_application_service_.BuildSessionResponse(*ns, session_id);
  if (!payload.has_value()) {
    return Json(resp, 404, json{{"error", "Session not found"}}.dump());
  }
  return Json(resp, 200, payload->dump());
}

int ServerApp::HandleCodeAgentSessionMessages(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "messages") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }

  const int limit = std::clamp(ParseInt(QueryOf(req, "limit"), 50), 1, 200);
  std::optional<int> before_seq = std::nullopt;
  const std::string before_raw = util::Trim(QueryOf(req, "beforeSeq"));
  if (!before_raw.empty()) {
    const int parsed = ParseInt(before_raw, 0);
    if (parsed > 0) {
      before_seq = parsed;
    }
  }

  const auto payload = codeagent_application_service_.BuildMessagesResponse(*ns, session_id, limit, before_seq);
  if (!payload.has_value()) {
    return Json(resp, 404, json{{"error", "Session not found"}}.dump());
  }
  return Json(resp, 200, payload->dump());
}

int ServerApp::HandleCodeAgentSessionResume(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "resume") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }

  std::string error;
  if (!codeagent_application_service_.ResumeSession(*ns, session_id, &error)) {
    return Json(resp, 404, json{{"error", error.empty() ? "Session not found" : error}}.dump());
  }
  return Json(resp, 200, json{{"type", "success"}, {"sessionId", session_id}}.dump());
}

int ServerApp::HandleCodeAgentSessionSendMessage(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "messages") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }

  const auto payload = ParseJsonOrNull(req->body);
  if (!payload.has_value() || !payload->is_object()) {
    return Json(resp, 400, json{{"error", "Invalid body"}}.dump());
  }

  const auto text_it = payload->find("text");
  if (text_it == payload->end() || !text_it->is_string()) {
    return Json(resp, 400, json{{"error", "Invalid body"}}.dump());
  }
  const std::string text = text_it->get<std::string>();

  std::string local_id;
  const auto local_id_it = payload->find("localId");
  if (local_id_it != payload->end()) {
    if (!local_id_it->is_string()) {
      return Json(resp, 400, json{{"error", "Invalid body"}}.dump());
    }
    local_id = util::Trim(local_id_it->get<std::string>());
  }

  json attachments = json::array();
  const auto attachments_it = payload->find("attachments");
  if (attachments_it != payload->end()) {
    if (!attachments_it->is_array()) {
      return Json(resp, 400, json{{"error", "Invalid body"}}.dump());
    }
    attachments = *attachments_it;
  }

  if (util::Trim(text).empty() && attachments.empty()) {
    return Json(resp, 400, json{{"error", "Message requires text or attachments"}}.dump());
  }

  std::string error;
  if (!codeagent_application_service_.SendSessionMessage(*ns, session_id, text, local_id, attachments, &error)) {
    return Json(resp, 400, json{{"error", error.empty() ? "failed to send message" : error}}.dump());
  }
  return Json(resp, 200, json{{"ok", true}}.dump());
}

int ServerApp::HandleCodeAgentSessionAbort(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "abort") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }

  std::string error;
  if (!codeagent_application_service_.AbortSession(*ns, session_id, &error)) {
    return Json(resp, 404, json{{"error", error.empty() ? "Session not found" : error}}.dump());
  }
  return Json(resp, 200, json{{"ok", true}}.dump());
}

int ServerApp::HandleCodeAgentSessionArchive(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "archive") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }

  std::string error;
  if (!codeagent_application_service_.ArchiveSession(*ns, session_id, &error)) {
    return Json(resp, 404, json{{"error", error.empty() ? "Session not found" : error}}.dump());
  }
  return Json(resp, 200, json{{"ok", true}}.dump());
}

int ServerApp::HandleCodeAgentSessionRename(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || !tail.empty()) {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string name = util::Trim(JsonString(payload, "name"));
  if (name.empty()) {
    return Json(resp, 400, json{{"error", "Invalid body: name is required"}}.dump());
  }

  std::string error;
  if (!codeagent_application_service_.RenameSession(*ns, session_id, name, &error)) {
    return Json(resp, 404, json{{"error", error.empty() ? "Session not found" : error}}.dump());
  }
  return Json(resp, 200, json{{"ok", true}}.dump());
}

int ServerApp::HandleCodeAgentSessionDelete(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || !tail.empty()) {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }

  std::string error;
  if (!codeagent_application_service_.DeleteSession(*ns, session_id, &error)) {
    return Json(resp, 404, json{{"error", error.empty() ? "Session not found" : error}}.dump());
  }
  return Json(resp, 200, json{{"ok", true}}.dump());
}

int ServerApp::HandleCodeAgentSessionPermissionApprove(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 3 || tail[0] != "permissions" ||
      tail[2] != "approve") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }
  const std::string request_id = tail[1];
  const auto payload = ParseJsonOrNull(req->body);
  const std::string mode = util::Trim(JsonString(payload, "mode"));
  const std::string decision = util::Trim(JsonString(payload, "decision"));

  std::vector<std::string> allow_tools;
  nlohmann::json answers = nlohmann::json::object();

  if (payload.has_value() && payload->is_object()) {
    auto allow_tools_it = payload->find("allowTools");
    if (allow_tools_it != payload->end() && allow_tools_it->is_array()) {
      for (const auto& item : *allow_tools_it) {
        if (!item.is_string()) {
          continue;
        }
        const std::string normalized = util::Trim(item.get<std::string>());
        if (!normalized.empty()) {
          allow_tools.push_back(normalized);
        }
      }
    }

    auto answers_it = payload->find("answers");
    if (answers_it != payload->end() && answers_it->is_object()) {
      answers = *answers_it;
    }
  }

  std::string error;
  if (!codeagent_application_service_.ApprovePermissionRequest(*ns, session_id, request_id, mode, allow_tools, decision, answers,
                                                   &error)) {
    const std::string normalized_error = util::Trim(error);
    if (normalized_error == "session is inactive") {
      return Json(resp, 409, json{{"error", "Session is inactive"}}.dump());
    }
    if (normalized_error == "invalid permission mode for session flavor" || normalized_error == "invalid decision") {
      return Json(resp, 400, json{{"error", "Invalid body"}}.dump());
    }
    if (normalized_error == "request not found") {
      return Json(resp, 404, json{{"error", "Request not found"}}.dump());
    }
    return Json(resp, 404, json{{"error", normalized_error.empty() ? "Session not found" : normalized_error}}.dump());
  }
  return Json(resp, 200, json{{"ok", true}}.dump());
}

int ServerApp::HandleCodeAgentSessionPermissionDeny(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 3 || tail[0] != "permissions" ||
      tail[2] != "deny") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }
  const std::string request_id = tail[1];
  const auto payload = ParseJsonOrNull(req->body);
  const std::string decision = util::Trim(JsonString(payload, "decision"));

  std::string error;
  if (!codeagent_application_service_.DenyPermissionRequest(*ns, session_id, request_id, decision, &error)) {
    const std::string normalized_error = util::Trim(error);
    if (normalized_error == "session is inactive") {
      return Json(resp, 409, json{{"error", "Session is inactive"}}.dump());
    }
    if (normalized_error == "invalid decision") {
      return Json(resp, 400, json{{"error", "Invalid body"}}.dump());
    }
    if (normalized_error == "request not found") {
      return Json(resp, 404, json{{"error", "Request not found"}}.dump());
    }
    return Json(resp, 404, json{{"error", normalized_error.empty() ? "Session not found" : normalized_error}}.dump());
  }
  return Json(resp, 200, json{{"ok", true}}.dump());
}

int ServerApp::HandleCodeAgentSessionPermissionMode(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "permission-mode") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string mode = util::Trim(JsonString(payload, "mode"));
  if (mode.empty()) {
    return Json(resp, 400, json{{"error", "Invalid body"}}.dump());
  }

  std::string error;
  if (!codeagent_application_service_.SetPermissionMode(*ns, session_id, mode, &error)) {
    const std::string normalized_error = util::Trim(error);
    if (normalized_error == "session is inactive") {
      return Json(resp, 409, json{{"error", "Session is inactive"}}.dump());
    }
    if (normalized_error == "invalid permission mode for session flavor") {
      return Json(resp, 400, json{{"error", "Invalid permission mode for session flavor"}}.dump());
    }
    return Json(resp, 404, json{{"error", normalized_error.empty() ? "Session not found" : normalized_error}}.dump());
  }

  std::string applied_mode = mode;
  auto session = codeagent_application_service_.BuildSessionResponse(*ns, session_id);
  if (session.has_value() && session->is_object()) {
    auto session_it = session->find("session");
    if (session_it != session->end() && session_it->is_object()) {
      auto mode_it = session_it->find("permissionMode");
      if (mode_it != session_it->end() && mode_it->is_string()) {
        applied_mode = mode_it->get<std::string>();
      }
    }
  }

  return Json(resp, 200, json{{"ok", true}, {"applied", {{"permissionMode", applied_mode}}}}.dump());
}

int ServerApp::HandleCodeAgentSessionModelMode(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "model") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string mode = util::Trim(JsonString(payload, "model"));
  if (mode.empty()) {
    return Json(resp, 400, json{{"error", "Invalid body"}}.dump());
  }

  std::string error;
  if (!codeagent_application_service_.SetModelMode(*ns, session_id, mode, &error)) {
    const std::string normalized_error = util::Trim(error);
    if (normalized_error == "session is inactive") {
      return Json(resp, 409, json{{"error", "Session is inactive"}}.dump());
    }
    if (normalized_error == "model mode is only supported for claude sessions") {
      return Json(resp, 400, json{{"error", "Model mode is only supported for Claude sessions"}}.dump());
    }
    if (normalized_error == "invalid model mode") {
      return Json(resp, 400, json{{"error", "Invalid body"}}.dump());
    }
    return Json(resp, 404, json{{"error", normalized_error.empty() ? "Session not found" : normalized_error}}.dump());
  }

  std::string applied_mode = mode;
  auto session = codeagent_application_service_.BuildSessionResponse(*ns, session_id);
  if (session.has_value() && session->is_object()) {
    auto session_it = session->find("session");
    if (session_it != session->end() && session_it->is_object()) {
      auto mode_it = session_it->find("modelMode");
      if (mode_it != session_it->end() && mode_it->is_string()) {
        applied_mode = mode_it->get<std::string>();
      }
    }
  }

  return Json(resp, 200, json{{"ok", true}, {"applied", {{"modelMode", applied_mode}}}}.dump());
}

int ServerApp::HandleCodeAgentSessionReasoningEffort(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "reasoning-effort") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string effort = util::Trim(JsonString(payload, "effort"));
  if (effort.empty()) {
    return Json(resp, 400, json{{"error", "Invalid body"}}.dump());
  }

  std::string error;
  if (!codeagent_application_service_.SetModelReasoningEffort(*ns, session_id, effort, &error)) {
    const std::string normalized_error = util::Trim(error);
    if (normalized_error == "session is inactive") {
      return Json(resp, 409, json{{"error", "Session is inactive"}}.dump());
    }
    if (normalized_error == "model reasoning effort is only supported for codex sessions") {
      return Json(resp, 400, json{{"error", "Model reasoning effort is only supported for Codex sessions"}}.dump());
    }
    if (normalized_error == "invalid model reasoning effort") {
      return Json(resp, 400, json{{"error", "Invalid body"}}.dump());
    }
    return Json(resp, 404, json{{"error", normalized_error.empty() ? "Session not found" : normalized_error}}.dump());
  }
  return Json(resp, 200, json{{"ok", true}}.dump());
}

int ServerApp::HandleCodeAgentSessionSlashCommands(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }
  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "slash-commands") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }
  return Json(resp, 200, codeagent_application_service_.BuildSlashCommandsResponse(*ns, session_id).dump());
}

int ServerApp::HandleCodeAgentSessionSkills(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }
  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "skills") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }
  return Json(resp, 200, codeagent_application_service_.BuildSkillsResponse(*ns, session_id).dump());
}

int ServerApp::HandleCodeAgentSessionGitStatus(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }
  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "git-status") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }
  const auto session_path = ResolveCodeAgentSessionPath(*ns, session_id, resp);
  if (!session_path.has_value()) {
    return resp->status_code;
  }

  const std::string command =
      "cd " + EscapeForSingleQuotedShell(session_path->string()) + " && git status --porcelain=v1 --branch 2>&1";
  int exit_code = -1;
  const std::string output = RunCommandCaptureWithExitCode(command, &exit_code);
  json result = {
      {"success", exit_code == 0},
      {"stdout", output},
      {"exitCode", exit_code},
  };
  if (exit_code != 0) {
    result["stderr"] = output;
  }
  return Json(resp, 200, result.dump());
}

int ServerApp::HandleCodeAgentSessionGitDiffNumstat(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }
  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "git-diff-numstat") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }
  const auto session_path = ResolveCodeAgentSessionPath(*ns, session_id, resp);
  if (!session_path.has_value()) {
    return resp->status_code;
  }

  const bool staged = ParseBool(QueryOf(req, "staged"), false);
  std::string command =
      "cd " + EscapeForSingleQuotedShell(session_path->string()) + " && git diff --numstat";
  if (staged) {
    command += " --staged";
  }
  command += " 2>&1";

  int exit_code = -1;
  const std::string output = RunCommandCaptureWithExitCode(command, &exit_code);
  json result = {
      {"success", exit_code == 0},
      {"stdout", output},
      {"exitCode", exit_code},
  };
  if (exit_code != 0) {
    result["stderr"] = output;
  }
  return Json(resp, 200, result.dump());
}

int ServerApp::HandleCodeAgentSessionGitDiffFile(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }
  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "git-diff-file") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }
  const auto session_path = ResolveCodeAgentSessionPath(*ns, session_id, resp);
  if (!session_path.has_value()) {
    return resp->status_code;
  }

  const std::string file_path = util::Trim(QueryOf(req, "path"));
  if (file_path.empty()) {
    return Json(resp, 400, json{{"error", "Invalid file path"}}.dump());
  }
  const bool staged = ParseBool(QueryOf(req, "staged"), false);
  std::string command = "cd " + EscapeForSingleQuotedShell(session_path->string()) + " && git diff";
  if (staged) {
    command += " --staged";
  }
  command += " -- " + EscapeForDoubleQuotedShell(file_path) + " 2>&1";

  int exit_code = -1;
  const std::string output = RunCommandCaptureWithExitCode(command, &exit_code);
  json result = {
      {"success", exit_code == 0},
      {"stdout", output},
      {"exitCode", exit_code},
  };
  if (exit_code != 0) {
    result["stderr"] = output;
  }
  return Json(resp, 200, result.dump());
}

int ServerApp::HandleCodeAgentSessionFileRead(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }
  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "file") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }
  const auto session_path = ResolveCodeAgentSessionPath(*ns, session_id, resp);
  if (!session_path.has_value()) {
    return resp->status_code;
  }

  const std::string request_path = util::Trim(QueryOf(req, "path"));
  if (request_path.empty()) {
    return Json(resp, 400, json{{"error", "Invalid file path"}}.dump());
  }
  const auto resolved = ResolvePathUnderRoot(*session_path, request_path);
  if (!resolved.has_value()) {
    return Json(resp, 400, json{{"success", false}, {"error", "Path is not allowed"}}.dump());
  }

  std::error_code ec;
  if (!std::filesystem::exists(*resolved, ec) || std::filesystem::is_directory(*resolved, ec)) {
    return Json(resp, 404, json{{"success", false}, {"error", "File not found"}}.dump());
  }

  return Json(resp, 200, json{{"success", true}, {"content", ReadFileText(*resolved)}}.dump());
}

int ServerApp::HandleCodeAgentSessionFileSearch(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }
  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "files") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }
  const auto session_path = ResolveCodeAgentSessionPath(*ns, session_id, resp);
  if (!session_path.has_value()) {
    return resp->status_code;
  }

  const std::string query = ToLower(util::Trim(QueryOf(req, "query")));
  const int limit = std::clamp(ParseInt(QueryOf(req, "limit"), 200), 1, 500);

  json files = json::array();
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(
      *session_path, std::filesystem::directory_options::skip_permission_denied, ec);
  std::filesystem::recursive_directory_iterator end;

  for (; it != end && static_cast<int>(files.size()) < limit; it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    const auto status = it->symlink_status(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    const bool is_file = std::filesystem::is_regular_file(status);
    const bool is_dir = std::filesystem::is_directory(status);
    if (!is_file && !is_dir) {
      continue;
    }

    const auto rel = std::filesystem::relative(it->path(), *session_path, ec);
    if (ec || rel.empty()) {
      ec.clear();
      continue;
    }
    const std::string full_path = rel.generic_string();
    if (!query.empty() && ToLower(full_path).find(query) == std::string::npos) {
      continue;
    }

    files.push_back({
        {"fileName", it->path().filename().string()},
        {"filePath", rel.parent_path().generic_string()},
        {"fullPath", full_path},
        {"fileType", is_dir ? "folder" : "file"},
    });
  }

  return Json(resp, 200, json{{"success", true}, {"files", files}}.dump());
}

int ServerApp::HandleCodeAgentSessionDirectory(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }
  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "directory") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }
  const auto session_path = ResolveCodeAgentSessionPath(*ns, session_id, resp);
  if (!session_path.has_value()) {
    return resp->status_code;
  }

  const auto resolved = ResolvePathUnderRoot(*session_path, QueryOf(req, "path"));
  if (!resolved.has_value()) {
    return Json(resp, 400, json{{"success", false}, {"error", "Path is not allowed"}}.dump());
  }

  std::error_code ec;
  if (!std::filesystem::exists(*resolved, ec) || !std::filesystem::is_directory(*resolved, ec)) {
    return Json(resp, 404, json{{"success", false}, {"error", "Directory not found"}}.dump());
  }

  std::vector<json> entries;
  for (std::filesystem::directory_iterator it(*resolved, std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       it != end; it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    const auto status = it->symlink_status(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    const bool is_file = std::filesystem::is_regular_file(status);
    const bool is_dir = std::filesystem::is_directory(status);
    json item = {
        {"name", it->path().filename().string()},
        {"type", is_dir ? "directory" : (is_file ? "file" : "other")},
    };
    if (is_file) {
      item["size"] = static_cast<std::uint64_t>(std::filesystem::file_size(it->path(), ec));
      if (ec) {
        ec.clear();
      }
    }
    entries.push_back(std::move(item));
  }

  std::sort(entries.begin(), entries.end(), [](const json& left, const json& right) {
    const std::string left_type = left.value("type", "");
    const std::string right_type = right.value("type", "");
    if (left_type != right_type) {
      return left_type < right_type;
    }
    return left.value("name", "") < right.value("name", "");
  });

  json out_entries = json::array();
  for (const auto& entry : entries) {
    out_entries.push_back(entry);
  }
  return Json(resp, 200, json{{"success", true}, {"entries", out_entries}}.dump());
}

int ServerApp::HandleCodeAgentSessionUpload(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }
  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 1 || tail[0] != "upload") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }
  const auto session_path = ResolveCodeAgentSessionPath(*ns, session_id, resp);
  if (!session_path.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string filename = SanitizeUploadFilename(JsonString(payload, "filename"));
  const std::string content = JsonString(payload, "content");
  if (content.empty()) {
    return Json(resp, 400, json{{"success", false}, {"error", "Invalid body"}}.dump());
  }
  if (EstimateBase64Bytes(content) > 50ULL * 1024ULL * 1024ULL) {
    return Json(resp, 413, json{{"success", false}, {"error", "File too large (max 50MB)"}}.dump());
  }

  const std::string decoded = util::Base64Decode(content);
  const std::filesystem::path upload_dir = *session_path / ".ferryman-codeagent-uploads";
  std::error_code ec;
  std::filesystem::create_directories(upload_dir, ec);
  if (ec) {
    return Json(resp, 500, json{{"success", false}, {"error", "Failed to create upload directory"}}.dump());
  }

  const std::filesystem::path target_path = upload_dir / (util::RandomHex(8) + "-" + filename);
  std::ofstream output(target_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    return Json(resp, 500, json{{"success", false}, {"error", "Failed to save upload"}}.dump());
  }
  output.write(decoded.data(), static_cast<std::streamsize>(decoded.size()));
  output.close();

  std::string relative_path = target_path.filename().string();
  const auto rel = std::filesystem::relative(target_path, *session_path, ec);
  if (!ec && !rel.empty()) {
    relative_path = rel.generic_string();
  }

  return Json(resp, 200, json{{"success", true}, {"path", relative_path}}.dump());
}

int ServerApp::HandleCodeAgentSessionUploadDelete(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }
  std::string session_id;
  std::vector<std::string> tail;
  if (!ParseApiSessionPath(req->path, &session_id, &tail) || tail.size() != 2 || tail[0] != "upload" ||
      tail[1] != "delete") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }
  const auto session_path = ResolveCodeAgentSessionPath(*ns, session_id, resp);
  if (!session_path.has_value()) {
    return resp->status_code;
  }

  const auto payload = ParseJsonOrNull(req->body);
  const std::string request_path = util::Trim(JsonString(payload, "path"));
  if (request_path.empty()) {
    return Json(resp, 400, json{{"success", false}, {"error", "Invalid body"}}.dump());
  }

  const auto target = ResolvePathUnderRoot(*session_path, request_path);
  if (!target.has_value()) {
    return Json(resp, 400, json{{"success", false}, {"error", "Path is not allowed"}}.dump());
  }

  std::error_code ec;
  const auto rel = std::filesystem::relative(*target, *session_path, ec);
  if (ec || rel.empty() || rel.begin() == rel.end() || (*rel.begin()) != ".ferryman-codeagent-uploads") {
    return Json(resp, 403, json{{"success", false}, {"error", "Delete denied"}}.dump());
  }

  std::filesystem::remove(*target, ec);
  if (ec) {
    return Json(resp, 500, json{{"success", false}, {"error", "Failed to delete upload"}}.dump());
  }
  return Json(resp, 200, json{{"success", true}}.dump());
}

int ServerApp::HandleCodeAgentMachines(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }
  return Json(resp, 200, codeagent_application_service_.BuildMachinesResponse(*ns).dump());
}

int ServerApp::HandleCodeAgentMachineSpawn(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  std::string machine_id;
  std::vector<std::string> tail;
  if (!ParseApiMachinePath(req->path, &machine_id, &tail) || tail.size() != 1 || tail[0] != "spawn") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }

  const json machines_payload = codeagent_application_service_.BuildMachinesResponse(*ns);
  bool machine_exists = false;
  if (machines_payload.contains("machines") && machines_payload["machines"].is_array()) {
    for (const auto& machine : machines_payload["machines"]) {
      if (machine.value("id", "") == machine_id) {
        machine_exists = true;
        break;
      }
    }
  }
  if (!machine_exists) {
    return Json(resp, 404, json{{"error", "Machine not found"}}.dump());
  }

  const auto payload = ParseJsonOrNull(req->body);
  codeagent::SpawnSessionOptions options;
  options.directory = JsonString(payload, "directory");
  options.agent = JsonString(payload, "agent", "claude");
  options.model = JsonString(payload, "model");
  options.permission_mode = JsonString(payload, "permissionMode");

  const auto result = codeagent_application_service_.SpawnSession(*ns, options);
  if (result.type != "success") {
    return Json(resp, 400, json{{"type", "error"}, {"message", result.message}}.dump());
  }
  return Json(resp, 200, json{{"type", "success"}, {"sessionId", result.session_id}}.dump());
}

int ServerApp::HandleCodeAgentMachinePathsExists(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  std::string machine_id;
  std::vector<std::string> tail;
  if (!ParseApiMachinePath(req->path, &machine_id, &tail) || tail.size() != 2 || tail[0] != "paths" ||
      tail[1] != "exists") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }

  const json machines_payload = codeagent_application_service_.BuildMachinesResponse(*ns);
  bool machine_exists = false;
  if (machines_payload.contains("machines") && machines_payload["machines"].is_array()) {
    for (const auto& machine : machines_payload["machines"]) {
      if (machine.value("id", "") == machine_id) {
        machine_exists = true;
        break;
      }
    }
  }
  if (!machine_exists) {
    return Json(resp, 404, json{{"error", "Machine not found"}}.dump());
  }

  const auto payload = ParseJsonOrNull(req->body);
  if (!payload.has_value() || !payload->contains("paths") || !(*payload)["paths"].is_array()) {
    return Json(resp, 400, json{{"error", "Invalid body"}}.dump());
  }

  std::vector<std::string> paths;
  for (const auto& item : (*payload)["paths"]) {
    if (item.is_string()) {
      paths.push_back(item.get<std::string>());
    }
  }

  json exists = json::object();
  std::string error;
  if (!codeagent_application_service_.CheckPathsExist(*ns, paths, &exists, &error)) {
    return Json(resp, 500, json{{"error", error.empty() ? "Failed to check paths" : error}}.dump());
  }
  return Json(resp, 200, json{{"exists", exists}}.dump());
}

int ServerApp::HandleCodeAgentMachineDirectory(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }

  std::string machine_id;
  std::vector<std::string> tail;
  if (!ParseApiMachinePath(req->path, &machine_id, &tail) || tail.size() != 1 || tail[0] != "directory") {
    return Json(resp, 404, json{{"error", "Not found"}}.dump());
  }

  const json machines_payload = codeagent_application_service_.BuildMachinesResponse(*ns);
  bool machine_exists = false;
  if (machines_payload.contains("machines") && machines_payload["machines"].is_array()) {
    for (const auto& machine : machines_payload["machines"]) {
      if (machine.value("id", "") == machine_id) {
        machine_exists = true;
        break;
      }
    }
  }
  if (!machine_exists) {
    return Json(resp, 404, json{{"error", "Machine not found"}}.dump());
  }

  const std::string raw_path = util::Trim(QueryOf(req, "path"));
  std::error_code ec;
  std::filesystem::path target;
  if (raw_path.empty()) {
    target = config_.workspace_root;
  } else {
    target = std::filesystem::path(raw_path);
    if (target.is_relative()) {
      target = std::filesystem::absolute(target, ec);
      if (ec) {
        return Json(resp, 400, json{{"success", false}, {"error", "Invalid path"}}.dump());
      }
    }
  }

  target = std::filesystem::weakly_canonical(target, ec);
  if (ec || target.empty()) {
    return Json(resp, 400, json{{"success", false}, {"error", "Invalid path"}}.dump());
  }
  if (!std::filesystem::exists(target, ec) || !std::filesystem::is_directory(target, ec)) {
    return Json(resp, 404, json{{"success", false}, {"error", "Directory not found"}}.dump());
  }

  std::vector<json> entries;
  for (std::filesystem::directory_iterator it(target, std::filesystem::directory_options::skip_permission_denied, ec),
       end;
       it != end; it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    const auto status = it->symlink_status(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    const bool is_file = std::filesystem::is_regular_file(status);
    const bool is_dir = std::filesystem::is_directory(status);
    json item = {
        {"name", it->path().filename().string()},
        {"type", is_dir ? "directory" : (is_file ? "file" : "other")},
    };
    if (is_file) {
      item["size"] = static_cast<std::uint64_t>(std::filesystem::file_size(it->path(), ec));
      if (ec) {
        ec.clear();
      }
    }
    entries.push_back(std::move(item));
  }

  std::sort(entries.begin(), entries.end(), [](const json& left, const json& right) {
    const std::string left_type = left.value("type", "");
    const std::string right_type = right.value("type", "");
    if (left_type != right_type) {
      if (left_type == "directory") {
        return true;
      }
      if (right_type == "directory") {
        return false;
      }
      return left_type < right_type;
    }
    return left.value("name", "") < right.value("name", "");
  });

  json out_entries = json::array();
  for (const auto& entry : entries) {
    out_entries.push_back(entry);
  }
  return Json(resp, 200, json{{"success", true}, {"path", target.string()}, {"entries", out_entries}}.dump());
}

int ServerApp::HandleCodeAgentPushVapidPublicKey(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }
  return Json(resp, 200, json{{"publicKey", ""}}.dump());
}

int ServerApp::HandleCodeAgentPushSubscribe(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }
  return Json(resp, 200, json{{"ok", true}}.dump());
}

int ServerApp::HandleCodeAgentVoiceToken(HttpRequest* req, HttpResponse* resp) {
  auto ns = RequireCodeAgentNamespace(req, resp);
  if (!ns.has_value()) {
    return resp->status_code;
  }
  return Json(resp, 200, json{{"allowed", false}, {"error", "Voice assistant is disabled"}}.dump());
}

int ServerApp::HandleCodeAgentAsset(HttpRequest* req, HttpResponse* resp) {
  std::string relative = req->path;
  if (relative.rfind("/codeagent", 0) != 0) {
    return Text(resp, 404, "Not Found");
  }

  relative = relative.substr(std::string("/codeagent").size());
  if (relative.empty() || relative == "/") {
    relative = "/index.html";
  }
  if (!relative.empty() && relative.front() == '/') {
    relative.erase(relative.begin());
  }

  std::filesystem::path relative_path = std::filesystem::path(relative).lexically_normal();
  if (relative_path.empty()) {
    relative_path = "index.html";
  }
  if (!IsSafeRelativePath(relative_path)) {
    return Text(resp, 403, "Forbidden");
  }

  // Prefer Ferryman embedded frontend assets so CodeAgent panel always matches
  // the integrated UI build and does not depend on legacy external bundles.
  const std::string embedded_path = "/" + relative_path.generic_string();
  auto embedded_asset = FindEmbeddedAsset(embedded_path);
  if (embedded_asset.has_value()) {
    return Text(resp, 200, std::string(embedded_asset->content), std::string(embedded_asset->mime_type));
  }

  auto embedded_index = FindEmbeddedAsset("/index.html");
  if (embedded_index.has_value()) {
    return Text(resp, 200, std::string(embedded_index->content), "text/html; charset=utf-8");
  }

  return Text(resp, 503,
              "CodeAgent web assets not found.\n\n"
              "Build frontend assets and rebuild backend first:\n"
              "  make frontend\n"
              "  make release\n");
}
