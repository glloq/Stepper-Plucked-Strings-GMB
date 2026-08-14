// Minimal ESPAsyncWebServer stub for host compile-checking.
#pragma once
#include <Arduino.h>
#include <functional>

namespace fs { class FS; }

enum WebRequestMethod { HTTP_GET = 1, HTTP_POST = 2, HTTP_PUT = 4, HTTP_DELETE = 8 };
enum AwsEventType { WS_EVT_CONNECT, WS_EVT_DISCONNECT, WS_EVT_DATA, WS_EVT_PONG, WS_EVT_ERROR };

class AsyncWebParameter {
public:
  String value() const { return String(""); }
};

class AsyncWebServerResponse {
public:
  void addHeader(const String&, const String&) {}
};

class AsyncWebServerRequest {
public:
  void send(int, const String&, const String&) {}
  void send(AsyncWebServerResponse*) {}
  AsyncWebServerResponse* beginResponse(int, const String&, const uint8_t*, size_t) {
    static AsyncWebServerResponse r;
    return &r;
  }
  void redirect(const String&) {}
  String url() const { return String("/"); }
  bool hasHeader(const char*) const { return false; }
  String header(const char*) const { return String(""); }
  bool hasParam(const char*) const { return false; }
  AsyncWebParameter* getParam(const char*) const { return nullptr; }
};

using ArRequestHandlerFunction = std::function<void(AsyncWebServerRequest*)>;

class AsyncWebHandler {};

class AsyncWebServerResponseStub {
public:
  AsyncWebServerResponseStub& setDefaultFile(const char*) { return *this; }
};

class AsyncWebSocketClient {};

class AsyncWebSocket : public AsyncWebHandler {
public:
  using EventHandler = std::function<void(AsyncWebSocket*, AsyncWebSocketClient*,
                                          AwsEventType, void*, uint8_t*, size_t)>;
  AsyncWebSocket(const char*) {}
  void onEvent(EventHandler) {}
  size_t count() const { return 0; }
  void textAll(const String&) {}
};

class AsyncWebServer {
public:
  AsyncWebServer(uint16_t) {}
  void on(const char*, WebRequestMethod, ArRequestHandlerFunction) {}
  void addHandler(AsyncWebHandler*) {}
  AsyncWebServerResponseStub& serveStatic(const char*, fs::FS&, const char*) {
    static AsyncWebServerResponseStub r;
    return r;
  }
  void onNotFound(ArRequestHandlerFunction) {}
  void begin() {}
};
