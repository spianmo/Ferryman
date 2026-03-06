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
  std::string token = util::Trim(HeaderOf(req, "X-Session-Token"));
  if (token.empty()) {
    token = util::Trim(HeaderOf(req, "x-session-token"));
  }
  if (token.empty()) {
    std::string authorization = util::Trim(HeaderOf(req, "Authorization"));
    if (authorization.empty()) {
      authorization = util::Trim(HeaderOf(req, "authorization"));
    }
    const std::string lower = ToLower(authorization);
    if (lower.rfind("bearer ", 0) == 0) {
      token = util::Trim(authorization.substr(7));
    }
  }
  if (token.empty()) {
    std::string cookie = HeaderOf(req, "Cookie");
    if (cookie.empty()) {
      cookie = HeaderOf(req, "cookie");
    }
    token = util::Trim(CookieValue(cookie, "ferryman_session_token"));
  }
  if (token.empty()) {
    token = util::Trim(QueryOf(req, "token"));
  }
  if (token.empty()) {
    Json(resp, 401, api::Error("missing session token", "unauthorized"));
    return std::nullopt;
  }

  auto session = auth_application_service_.GetSession(token);
  if (!session.has_value()) {
    Json(resp, 401, api::Error("invalid session token", "unauthorized"));
    return std::nullopt;
  }
  auth_application_service_.TouchSession(token);
  return session;
}

std::optional<std::string> ServerApp::RequireCodeAgentNamespace(HttpRequest* req, HttpResponse* resp,
                                                                std::string* bearer_token) {
  auto session = RequireSession(req, resp);
  if (!session.has_value()) {
    return std::nullopt;
  }

  if (bearer_token != nullptr) {
    *bearer_token = session->token;
  }
  return std::string("default");
}

std::optional<std::filesystem::path> ServerApp::ResolveCodeAgentSessionPath(const std::string& ns,
                                                                             const std::string& session_id,
                                                                             HttpResponse* resp) {
  const auto path = codeagent_application_service_.ResolveSessionPath(ns, session_id);
  if (!path.has_value()) {
    Json(resp, 404, json{{"error", "Session not found"}}.dump());
    return std::nullopt;
  }
  return path;
}

