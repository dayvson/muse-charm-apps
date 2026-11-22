#!/usr/bin/env bash
# Step 1: full factory backup of the Waveshare ESP32-S3-Touch-AMOLED-1.75C.
# Read-only against the board: nothing here writes to flash or eFuses.
# Usage: scripts/01_backup_factory.sh [PORT]
set -euo pipefail

PORT="${1:-${MUSE_PORT:-$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1)}}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/toolchain/tools-venv/bin"
OUT="$ROOT/backups"
TS="$(date +%Y%m%d-%H%M%S)"
BASE="$OUT/factory-$TS"
mkdir -p "$OUT"

echo "==> Chip / flash info"
"$BIN/esptool" --port "$PORT" flash-id | tee "$BASE-flash-id.txt"

echo "==> eFuse summary (read-only, for reference)"
"$BIN/espefuse" --port "$PORT" summary > "$BASE-efuse.txt" 2>&1 \
  || echo "   (eFuse summary failed; continuing)"

echo "==> Reading entire flash (32 MB can take several minutes)"
"$BIN/esptool" --port "$PORT" read-flash 0 ALL "$BASE.bin"

echo "==> Verifying the dump against the chip"
"$BIN/esptool" --port "$PORT" verify-flash 0 "$BASE.bin"

shasum -a 256 "$BASE.bin" | tee "$BASE.bin.sha256"
ls -lh "$BASE.bin"
echo
echo "Backup complete: $BASE.bin"
echo "Restore later with:"
echo "  $BIN/esptool --port $PORT write-flash 0 $BASE.bin"
