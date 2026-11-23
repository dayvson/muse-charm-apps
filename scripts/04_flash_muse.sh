#!/usr/bin/env bash
# Step 4: flash the Muse Gadget firmware (profile waveshare-s3-175c).
#   scripts/04_flash_muse.sh [PORT]            -> first flash: verify backup, ERASE, flash
#   scripts/04_flash_muse.sh --reflash [PORT]  -> later builds: flash only (keeps pairing/Wi-Fi)
#   add --avatar to rewrite the avatar's frames even if they look already flashed
# With your own avatar's frames in a flash partition (scripts/05_avatar.sh make),
# they're written to the "avatar" partition too, when they changed since the last flash.
# Coming from Waveshare's factory firmware, the SDK requires one full erase:
# Muse's NVS sits where the old firmware kept its data, and Muse refuses to
# wipe NVS on its own.
set -euo pipefail

MODE=first AVATAR_FORCE=
while [ $# -gt 0 ]; do
  case "$1" in
    --reflash) MODE=reflash; shift ;;
    --avatar) AVATAR_FORCE=1; shift ;;
    *) break ;;
  esac
done
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

# The avatar's frames, when the active avatar keeps them in the flash partition.
AV_SRC="$MUSE_ESP32_DIR/components/muse/avatar/muse_pixel.c"
AV_BIN="$MUSE_ESP32_DIR/components/muse/avatar/muse_pixel.bin"
AV_STAMP="$ROOT/logs/.avatar-flashed"
EXTRA=()
if [ -f "$AV_SRC" ] && grep -q '^#define GIF_EXTERNAL 1' "$AV_SRC"; then
  [ -f "$AV_BIN" ] || { echo "The avatar's frames ($AV_BIN) are missing: scripts/05_avatar.sh use ID" >&2; exit 1; }
  read -r AV_OFF AV_SIZE < <(awk -F, '$1 ~ /^ *avatar *$/ { gsub(/ /, "", $4); gsub(/ /, "", $5); print $4, $5 }' \
                              "$ROOT/config/partitions-muse-charm.csv")
  [ -n "${AV_OFF:-}" ] || { echo "No avatar partition in config/partitions-muse-charm.csv" >&2; exit 1; }
  case "$AV_SIZE" in *M) AV_SIZE=$(( ${AV_SIZE%M} * 1048576 )) ;; *K) AV_SIZE=$(( ${AV_SIZE%K} * 1024 )) ;; *) AV_SIZE=$((AV_SIZE)) ;; esac
  [ "$(stat -f %z "$AV_BIN")" -le "$AV_SIZE" ] || { echo "$AV_BIN is bigger than the avatar partition" >&2; exit 1; }
  AV_SHA="$(shasum -a 256 "$AV_BIN" | cut -c1-64)"
  if [ "$MODE" = first ] || [ -n "$AVATAR_FORCE" ] || [ "$(cat "$AV_STAMP" 2>/dev/null)" != "$AV_SHA" ]; then
    EXTRA=("$AV_OFF" "$AV_BIN")
    echo "==> The avatar's frames ($(( $(stat -f %z "$AV_BIN") / 1024 )) KB) go to the avatar partition at $AV_OFF"
  else
    echo "==> The avatar's frames are unchanged since the last flash (--avatar to rewrite them)"
  fi
fi

echo "==> Flashing bootloader, partition table (0x10000), otadata and app"
(cd "$BUILD" && "${ESPTOOL[@]}" -b 460800 --before default-reset --after hard-reset \
    write-flash "@flash_args" ${EXTRA[@]+"${EXTRA[@]}"})
if [ ${#EXTRA[@]} -gt 0 ]; then mkdir -p "$ROOT/logs" && echo "$AV_SHA" > "$AV_STAMP"; fi

echo
echo "Flashed. Capture the first boot with:"
echo "  . $ROOT/scripts/env.sh && python $MUSE_ESP32_DIR/tools/muse/monitor.py $PORT 30"
