#!/usr/bin/env bash
# Step 4: flash the Muse Gadget firmware (profile waveshare-s3-175c).
#   scripts/04_flash_muse.sh [PORT]            -> first flash: verify backup, ERASE, flash
#   scripts/04_flash_muse.sh --reflash [PORT]  -> later builds: flash only (keeps pairing/Wi-Fi)
# Coming from Waveshare's factory firmware, the SDK requires one full erase:
# Muse's NVS sits where the old firmware kept its data, and Muse refuses to
# wipe NVS on its own.
set -euo pipefail

MODE=first
if [ "${1:-}" = "--reflash" ]; then MODE=reflash; shift; fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/env.sh"
PORT="${1:-$MUSE_PORT}"
BUILD="$MUSE_ESP32_DIR/build-muse-waveshare-s3-175c"
ESPTOOL=(python -m esptool --chip esp32s3 -p "$PORT")

[ -f "$BUILD/flash_args" ] || { echo "No build found at $BUILD (run the build first)." >&2; exit 1; }

if [ "$MODE" = first ]; then
  echo "==> Checking for a verified factory backup"
  BK="$(ls -t "$ROOT"/backups/factory-*.bin 2>/dev/null | head -1 || true)"
  [ -n "$BK" ] || { echo "No backups/factory-*.bin found. Run scripts/01_backup_factory.sh first." >&2; exit 1; }
  SIZE=$(stat -f %z "$BK")
  [ "$SIZE" -eq 33554432 ] || { echo "$BK is $SIZE bytes, expected 33554432 (32 MB). Not erasing." >&2; exit 1; }
  (cd "$(dirname "$BK")" && shasum -a 256 -c "$(basename "$BK").sha256") \
    || { echo "Backup checksum mismatch. Not erasing." >&2; exit 1; }
  echo "   OK: $BK"
  echo
  echo "This will ERASE the whole 32 MB flash (Waveshare factory firmware) and install Muse."
  read -rp "Type ERASE to continue: " ans
  [ "$ans" = "ERASE" ] || { echo "Aborted."; exit 1; }
  echo "==> Erasing flash"
  "${ESPTOOL[@]}" erase-flash
fi

echo "==> Flashing bootloader, partition table (0x10000), otadata and app"
(cd "$BUILD" && "${ESPTOOL[@]}" -b 460800 --before default-reset --after hard-reset \
    write-flash "@flash_args")

echo
echo "Flashed. Capture the first boot with:"
echo "  . $ROOT/scripts/env.sh && python $MUSE_ESP32_DIR/tools/muse/monitor.py $PORT 30"
