#!/usr/bin/env bash
# Step 3: store your Muse Gadget SDK token in the board's NVS over serial.
# The token is read silently from the terminal, never written to disk or the
# firmware image, and redacted from anything echoed back.
# Usage: scripts/03_set_token.sh [PORT]     (run AFTER flashing the Muse firmware)
set -euo pipefail
PORT="${1:-${MUSE_PORT:-$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1)}}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="$ROOT/toolchain/tools-venv/bin/python"

read -rsp "Paste SDK token (mgst_..., input hidden): " TOKEN; echo
case "$TOKEN" in mgst_*) ;; *) echo "That doesn't look like an mgst_ token." >&2; exit 1;; esac

MUSE_TOKEN="$TOKEN" "$PY" - "$PORT" <<'EOF'
import os, re, sys, time, serial
port, token = sys.argv[1], os.environ.pop("MUSE_TOKEN")
redact = lambda s: re.sub(r"mgst_\S+", "mgst_<redacted>", s)
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dtr = s.rts = False          # don't reset the board when opening the port
s.open()
time.sleep(1.0); s.reset_input_buffer()
s.write(f">sdk_token={token}\n".encode()); s.flush()
deadline, buf = time.time() + 10, ""
while time.time() < deadline:
    buf += s.read(512).decode(errors="replace")
    if "@sdk_token ok" in buf:
        print("Board replied: @sdk_token ok -- token stored in NVS; board will restart.")
        sys.exit(0)
    m = re.search(r"@sdk_token[^\n]*", buf)
    if m and "ok" not in m.group(0):
        print("Board replied:", redact(m.group(0))); sys.exit(2)
print("No '@sdk_token ok' within 10 s. Last output:\n" + redact(buf[-800:])); sys.exit(3)
EOF
