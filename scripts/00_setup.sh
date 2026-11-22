#!/usr/bin/env bash
# Step 0: everything this project needs, inside this folder (nothing global):
#   muse-gadget-sdk/          the Muse Gadget SDK at the tested commit, with patches/ applied
#                             and firmware/ linked in as components/muse/apps
#   toolchain/esp-idf-v6.0.3  ESP-IDF, with its tools in toolchain/espressif (ESP32-S3 only)
#   toolchain/tools-venv      Python for the scripts: esptool, pyserial, Pillow, imageio-ffmpeg
# Safe to run again: finished steps are skipped.
# Usage: scripts/00_setup.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK_URL=https://github.com/facebookincubator/muse-gadget-sdk.git
SDK_COMMIT=b11908c        # the SDK commit patches/ were made against
IDF_VERSION=v6.0.3
IDF="$ROOT/toolchain/esp-idf-$IDF_VERSION"
export IDF_TOOLS_PATH="$ROOT/toolchain/espressif"
VENV="$ROOT/toolchain/tools-venv"
# Behind a TLS-inspecting proxy, set MUSE_CA_BUNDLE to its CA bundle before running this.
CA="${MUSE_CA_BUNDLE:-/opt/facebook/certs/facebook-ca-bundle.cer}"
if [ -f "$CA" ]; then export PIP_CERT="$CA" SSL_CERT_FILE="$CA" REQUESTS_CA_BUNDLE="$CA"; fi

for t in git python3 cc; do command -v "$t" >/dev/null || { echo "Missing $t. On macOS: xcode-select --install (and python3 3.10+)." >&2; exit 1; }; done
mkdir -p "$ROOT/toolchain"

echo "==> Muse Gadget SDK ($SDK_COMMIT)"
SDK="$ROOT/muse-gadget-sdk"
if [ ! -d "$SDK/.git" ]; then
  git clone "$SDK_URL" "$SDK"
  git -C "$SDK" checkout -q "$SDK_COMMIT"
fi
HEAD="$(git -C "$SDK" rev-parse --short=7 HEAD)"
[ "$HEAD" = "$SDK_COMMIT" ] || echo "   note: SDK is at $HEAD, patches were made against $SDK_COMMIT (they may not apply)"
for p in 0001-muse_vm-task-stack-via-helper_task.patch 0002-muse-apps-hooks.patch; do
  if git -C "$SDK" apply --reverse --check "$ROOT/patches/$p" 2>/dev/null; then
    echo "   $p already applied"
  else
    git -C "$SDK" apply "$ROOT/patches/$p" && echo "   applied $p"
  fi
done
ln -sfn ../../../../firmware "$SDK/esp32/components/muse/apps"
echo "   linked firmware/ as esp32/components/muse/apps"

echo "==> ESP-IDF $IDF_VERSION"
if [ ! -f "$IDF/export.sh" ]; then
  git clone -b "$IDF_VERSION" --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git "$IDF"
fi
"$IDF/install.sh" esp32s3 2>&1 | tee "$ROOT/toolchain/idf-install.log" | tail -2

echo "==> Python tools ($VENV)"
[ -x "$VENV/bin/python" ] || python3 -m venv "$VENV"
"$VENV/bin/pip" install -q --upgrade pip
"$VENV/bin/pip" install -q esptool pyserial Pillow imageio-ffmpeg
"$VENV/bin/python" -c "import esptool, serial, PIL, imageio_ffmpeg; print('   esptool, pyserial, Pillow, imageio-ffmpeg OK')"

cat <<EOF

Setup done. Next (docs/setup.md has the details):
  scripts/01_backup_factory.sh      back up the board's factory flash (do this first)
  scripts/05_avatar.sh status       optional: default avatar, or make your own
  scripts/02_build.sh               build the firmware
  scripts/04_flash_muse.sh          erase and flash (first time), then scripts/03_set_token.sh
EOF
