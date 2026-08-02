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
# Copy the app files (html/css/js), skip the docs README.
cp -R "$src/." "$dst/"
rm -f "$dst/README.md"

echo "Synced web-interface/ -> firmware/data/www"
find "$dst" -type f | sed "s#$here/##" | sort
