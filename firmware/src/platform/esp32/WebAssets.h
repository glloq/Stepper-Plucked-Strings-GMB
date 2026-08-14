// Web UI embedded in the firmware image, so a plain firmware upload (Arduino
// IDE "Upload" / `pio run -t upload`) ships a working interface with NO
// separate LittleFS-upload step.
//
// WebAssets.cpp is GENERATED from web-interface/ by
// firmware/tools/embed_web_assets.py (sync_web_data.sh runs it too) and is
// committed. LittleFS /www — when uploaded — still takes precedence per file
// (WebApi registers serveStatic first), so the UI can be updated on a device
// without recompiling the firmware.
#pragma once

#include <cstdint>

namespace gmb {

struct WebAsset {
    const char* path;     // request path ("/index.html", "/js/app.js", ...)
    const char* mime;     // Content-Type
    const uint8_t* data;  // body (gzip-compressed when gzip is true)
    uint32_t size;
    bool gzip;            // serve with Content-Encoding: gzip
};

// The embedded asset for `path`, or nullptr when none matches (linear scan —
// the table is small and this only runs on a LittleFS miss).
const WebAsset* findWebAsset(const char* path);

}  // namespace gmb
