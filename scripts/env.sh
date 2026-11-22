# Source this (". scripts/env.sh") to get ESP-IDF v6.0.3 from the project-local install.
_MC_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")/.." && pwd)"
export MUSE_CHARM_ROOT="$_MC_ROOT"
export IDF_PATH="$_MC_ROOT/toolchain/esp-idf-v6.0.3"
export IDF_TOOLS_PATH="$_MC_ROOT/toolchain/espressif"
export IDF_EXPORT="$IDF_PATH/export.sh"          # used by tools/muse/board.sh
# The board on USB: set MUSE_PORT to pick one when several are plugged in.
export MUSE_PORT="${MUSE_PORT:-$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1)}"
# Behind a TLS-inspecting proxy, set MUSE_CA_BUNDLE to its CA bundle (e.g. in ~/.zshrc).
_MC_CA="${MUSE_CA_BUNDLE:-/opt/facebook/certs/facebook-ca-bundle.cer}"   # used only if the file exists
if [ -f "$_MC_CA" ]; then
  export PIP_CERT="$_MC_CA" SSL_CERT_FILE="$_MC_CA" REQUESTS_CA_BUNDLE="$_MC_CA"
fi
. "$IDF_EXPORT" >/dev/null 2>&1 || { echo "ESP-IDF export failed" >&2; return 1; }
export MUSE_ESP32_DIR="$_MC_ROOT/muse-gadget-sdk/esp32"
unset _MC_ROOT _MC_CA
