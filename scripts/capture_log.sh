#!/usr/bin/env bash
# Stream the board's serial log to the terminal and to logs/serial-<ts>.log,
# WITHOUT resetting the board. Tokens/passwords are redacted in both.
# Usage: scripts/capture_log.sh [SECONDS=180] [PORT]      (Ctrl-C to stop early)
set -euo pipefail
SECS="${1:-180}"
PORT="${2:-${MUSE_PORT:-$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1)}}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/logs"
OUT="$ROOT/logs/serial-$(date +%Y%m%d-%H%M%S).log"
echo "Capturing $PORT for ${SECS}s -> $OUT"
"$ROOT/toolchain/tools-venv/bin/python" - "$PORT" "$SECS" "$OUT" <<'EOF'
import re, sys, time, serial
port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
red = lambda s: re.sub(r"(?i)(mgst_\S+|(pass(word)?|psk|token)\s*[=:]\s*\S+)", "<redacted>", s)
ansi = re.compile(r"\x1b\[[0-9;]*m")
s = serial.Serial(); s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dtr = s.rts = False            # don't reset on open
s.open()
end, pending = time.time() + secs, ""
with open(out, "w") as f:
    try:
        while time.time() < end:
            pending += s.read(4096).decode("utf-8", "replace")
            *lines, pending = pending.split("\n")
            for line in lines:
                line = red(ansi.sub("", line.rstrip("\r")))
                print(line, flush=True); f.write(line + "\n"); f.flush()
    except KeyboardInterrupt:
        pass
print(f"\nSaved: {out}")
EOF
