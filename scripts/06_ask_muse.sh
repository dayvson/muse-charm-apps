#!/usr/bin/env bash
# Send your Muse a typed message through the board and save the reply to
# logs/muse-reply-<timestamp>.txt (so Claude can read it).
# Usage: scripts/06_ask_muse.sh "message"      or      scripts/06_ask_muse.sh --file FILE
set -euo pipefail
[ $# -ge 1 ] || { echo "Usage: $0 \"message\" | --file FILE" >&2; exit 1; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/env.sh"
mkdir -p "$ROOT/logs"
OUT="$ROOT/logs/muse-reply-$(date +%Y%m%d-%H%M%S).txt"
cd "$MUSE_ESP32_DIR"
"$ROOT/toolchain/tools-venv/bin/python" tools/muse/chat.py --out "$OUT" "$@"
echo
echo "Saved reply: $OUT"
