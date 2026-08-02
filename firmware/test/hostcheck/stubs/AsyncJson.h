// Minimal AsyncJson stub for host compile-checking.
#pragma once
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <functional>

using ArJsonRequestHandlerFunction =
    std::function<void(AsyncWebServerRequest*, JsonVariant&)>;

class AsyncCallbackJsonWebHandler : public AsyncWebHandler {
public:
  AsyncCallbackJsonWebHandler(const char*, ArJsonRequestHandlerFunction) {}
  void setMethod(WebRequestMethod) {}
};
