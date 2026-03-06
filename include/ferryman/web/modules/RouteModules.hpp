#pragma once

#if !defined(FERRYMAN_WITH_LIBHV)
#define FERRYMAN_WITH_LIBHV 0
#endif

#if FERRYMAN_WITH_LIBHV
#include "hv/HttpServer.h"
#include "hv/WebSocketServer.h"
#endif

namespace ferryman::web {
class HttpController;
class WsController;
}

namespace ferryman::web::modules {

#if FERRYMAN_WITH_LIBHV

class AuthModule {
 public:
  explicit AuthModule(HttpController& controller) : controller_(controller) {}
  void Register(HttpService* http_service) const;

 private:
  HttpController& controller_;
};

class FileTaskModule {
 public:
  explicit FileTaskModule(HttpController& controller) : controller_(controller) {}
  void Register(HttpService* http_service) const;

 private:
  HttpController& controller_;
};

class DockurrModule {
 public:
  explicit DockurrModule(HttpController& controller) : controller_(controller) {}
  void Register(HttpService* http_service) const;

 private:
  HttpController& controller_;
};

class DockerModule {
 public:
  explicit DockerModule(HttpController& controller) : controller_(controller) {}
  void Register(HttpService* http_service) const;

 private:
  HttpController& controller_;
};

class ScreenModule {
 public:
  explicit ScreenModule(HttpController& controller) : controller_(controller) {}
  void Register(HttpService* http_service) const;

 private:
  HttpController& controller_;
};

class TunnelModule {
 public:
  explicit TunnelModule(HttpController& controller) : controller_(controller) {}
  void Register(HttpService* http_service) const;

 private:
  HttpController& controller_;
};

class CodeAgentModule {
 public:
  explicit CodeAgentModule(HttpController& controller) : controller_(controller) {}
  void Register(HttpService* http_service) const;

 private:
  HttpController& controller_;
};

class AssetModule {
 public:
  explicit AssetModule(HttpController& controller) : controller_(controller) {}
  void Register(HttpService* http_service) const;

 private:
  HttpController& controller_;
};

class WsModule {
 public:
  explicit WsModule(WsController& controller) : controller_(controller) {}
  void Register(hv::WebSocketService* ws_service) const;

 private:
  WsController& controller_;
};

#endif

}  // namespace ferryman::web::modules
