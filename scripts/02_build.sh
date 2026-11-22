#!/usr/bin/env bash
# Build Muse Gadget firmware for the Waveshare 1.75C with this project's
# settings (config/muse-charm.sdkconfig) merged into the build's sdkconfig.
# @ROOT@ in the overlay stands for this folder.
# Usage: scripts/02_build.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ -e "$ROOT/muse-gadget-sdk/esp32/components/muse/apps/muse_apps.c" ] \
  || { echo "The SDK isn't set up (or firmware/ isn't linked into it): run scripts/00_setup.sh" >&2; exit 1; }
. "$ROOT/scripts/env.sh"
"$ROOT/scripts/05_avatar.sh" status
cd "$MUSE_ESP32_DIR"
BUILD="build-muse-waveshare-s3-175c"
CFG="$BUILD/sdkconfig"
OVERLAY="$ROOT/config/muse-charm.sdkconfig"

# First build creates the sdkconfig from the SDK's defaults and overlays.
[ -f "$CFG" ] || tools/muse/board.sh build s3

echo "==> Applying $OVERLAY"
# Accepts "CONFIG_X=value" and "# CONFIG_X is not set" lines.
sed "s#@ROOT@#$ROOT#g" "$OVERLAY" | grep -E '^(CONFIG_[A-Z0-9_]+=|# CONFIG_[A-Z0-9_]+ is not set)' | while read -r line; do
  key="$(echo "$line" | grep -oE 'CONFIG_[A-Z0-9_]+')"
  if grep -qE "^($key=|# $key is not set)" "$CFG"; then
    sed -i '' -E "s|^($key=.*\|# $key is not set)\$|$line|" "$CFG"
  else
    echo "$line" >> "$CFG"
  fi
  echo "   $line"
done

tools/muse/board.sh build s3

echo "==> Effective settings:"
for key in $(grep -E '^(CONFIG_|# CONFIG_[A-Z0-9_]+ is not set)' "$OVERLAY" | grep -oE 'CONFIG_[A-Z0-9_]+'); do
  grep -E "^($key=|# $key is not set)" "$CFG" || echo "   $key missing!"
done
ls -l "$BUILD/muse-gadget.bin"
