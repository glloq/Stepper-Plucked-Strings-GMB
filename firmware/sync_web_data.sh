#!/usr/bin/env bash
# Copy the web interface into the Arduino/PlatformIO LittleFS image folder.
#
# The firmware serves the UI from LittleFS at /www. Both the Arduino IDE
# filesystem uploader and PlatformIO's `uploadfs` publish the sketch's `data/`
# folder, so the UI must be mirrored to firmware/data/www before uploading.
#
# Run this whenever web-interface/ changes:
#     cd firmware && ./sync_web_data.sh
#
# (The mirrored copy in data/www is generated and git-ignored; web-interface/
# stays the single source of truth.)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../web-interface"
dst="$here/data/www"

if [ ! -d "$src" ]; then
  echo "error: web-interface/ not found at $src" >&2
  exit 1
fi

rm -rf "$dst"
mkdir -p "$dst"
# Copy the app files (html/css/js) only: the README, the Node behavioural tests
# and the maintenance scripts are dev-only and must not eat LittleFS space.
# Keep this exclusion list in sync with SKIP/SKIP_DIRS in tools/embed_web_assets.py.
cp -R "$src/." "$dst/"
rm -f "$dst/README.md"
rm -rf "$dst/test" "$dst/tools"

echo "Synced web-interface/ -> firmware/data/www"
find "$dst" -type f | sed "s#$here/##" | sort

# Also regenerate the embedded copy compiled INTO the firmware (WebAssets.cpp,
# committed) so a plain Arduino IDE / PlatformIO firmware upload ships the same
# UI with no separate filesystem-upload step.
if command -v python3 >/dev/null 2>&1; then
  python3 "$here/tools/embed_web_assets.py"
else
  echo "warning: src/platform/esp32/WebAssets.cpp NOT regenerated (python3 missing)" >&2
fi
