#pragma once
#include <Arduino.h>
class MDNSStub { public: bool begin(const char*) { return true; } };
static MDNSStub MDNS;
