#!/usr/bin/env bash
# Screenshots of the Lua apps installed on the board, taken on the board itself.
# Over USB it opens each app (the serial console's "apps luapp.run" line), waits
# for it to draw and fetch its data, and saves the screen as a round PNG like the
# native apps' previews. Needs a firmware with CONFIG_LV_USE_SNAPSHOT (this
# project's overlay has it).
#
#   scripts/07_screenshots.sh [--wait SECONDS] [--out DIR] [ID ...]
#
# Without IDs it takes every installed app, plus the launcher. Defaults: --wait 8,
# --out docs/images/apps. Run it with the board awake and Muse connected (apps that
# fetch data need Wi-Fi). Muse's screen comes back at the end.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/env.sh"
WAIT=8
OUT="$ROOT/docs/images/apps"
IDS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --wait) WAIT="$2"; shift ;;
    --out) OUT="$2"; shift ;;
    -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) IDS+=("$1") ;;
  esac
  shift
done
[ -n "$MUSE_PORT" ] || { echo "No board on USB (set MUSE_PORT)." >&2; exit 1; }
mkdir -p "$OUT" "$ROOT/logs"
"$ROOT/toolchain/tools-venv/bin/python" -I - "$MUSE_PORT" "$WAIT" "$OUT" "$MUSE_ESP32_DIR/tools/muse" \
    "$ROOT/logs/screenshots-$(date +%Y%m%d-%H%M%S).log" ${IDS[@]+"${IDS[@]}"} <<'EOF'
import json, re, sys, time
port, wait, out, snapdir, logpath = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
ids = sys.argv[6:]
sys.path.insert(0, snapdir)
import serial
from snap import decode_snapshot           # the SDK's decoder (tools/muse/snap.py)
from PIL import Image, ImageDraw

s = serial.Serial(port, 115200, timeout=0.2)
try:   # closing drops DTR, which with RTS high resets the chip: keep them (as snap.py does)
    import termios
    a = termios.tcgetattr(s.fd); a[2] &= ~termios.HUPCL; termios.tcsetattr(s.fd, termios.TCSANOW, a)
except (ImportError, AttributeError, OSError):
    pass
log = open(logpath, "w", encoding="utf-8", errors="replace")

def read(secs, *until):
    end, buf = time.time() + secs, b""
    while time.time() < end and not any(u in buf for u in until):
        buf += s.read(65536)
    log.write(buf.decode("latin1"))
    return buf

def apps(command, params=None):
    """Runs an apps command over the console and returns its result."""
    read(0.3)
    s.write(f">apps {command} {json.dumps(params) if params else ''}\n".encode())
    buf = read(5, b"@apps ")
    buf += read(1, b"\n") if not re.search(rb"@apps [^\n]*\n", buf) else b""
    m = re.search(rb"@apps ([^\r\n]*)", buf)
    if not m:
        sys.exit(f"no answer to '{command}': is this firmware built with the apps console (patches/0002)?")
    return json.loads(m.group(1))

def screenshot(path):
    read(0.3)
    s.write(b"p")
    buf = read(15, b"SNAP END", b"SNAP OFF", b"SNAP BEGIN")
    if b"SNAP BEGIN" in buf and b"SNAP END" not in buf:
        buf += read(120, b"SNAP END")
    text = buf.decode("latin1")
    if "SNAP OFF" in text:
        sys.exit("screenshots are off in this firmware: rebuild with CONFIG_LV_USE_SNAPSHOT=y (config/muse-charm.sdkconfig)")
    w, h, raw = decode_snapshot(text)
    px = bytearray()
    for i in range(0, len(raw), 2):
        v = raw[i] | raw[i + 1] << 8
        px += bytes(((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3))
    shot = Image.frombytes("RGB", (w, h), bytes(px))
    # The screen is round: frame it like firmware/preview (grey outside the circle).
    frame = Image.new("RGB", (w, h), (40, 40, 40))
    mask = Image.new("L", (w * 4, h * 4), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, w * 4 - 1, h * 4 - 1), fill=255)
    frame.paste(shot, (0, 0), mask.resize((w, h), Image.LANCZOS))
    frame.save(path, optimize=True)
    print(f"   saved {path} ({w}x{h})")

listing = apps("luapp.admin", {"action": "list"})
if not listing.get("ok"):
    sys.exit(f"luapp.admin list failed: {listing}")
installed = {a["id"]: a["name"] for a in listing["payload"]["apps"]}
print("Installed:", ", ".join(f"{k} ({v})" for k, v in installed.items()) or "none")
todo = ids or list(installed)
missing = [i for i in todo if i not in installed]
if missing:
    sys.exit(f"not installed: {', '.join(missing)}")

print("==> launcher")
apps("luapp.admin", {"action": "stop"})
apps("app.show", {"app": "apps"})
read(2.5)
screenshot(f"{out}/launcher.png")

failed = []
for app_id in todo:
    print(f"==> {app_id} ({installed[app_id]}): opening, waiting {wait:g} s")
    apps("app.show", {"app": "apps"})
    apps("luapp.run", {"id": app_id})
    read(wait)
    running = apps("luapp.admin", {"action": "list"}).get("payload", {}).get("running")
    if running != app_id:
        err = apps("luapp.admin", {"action": "logs"}).get("payload", {}).get("error")
        print(f"   {app_id} isn't running ({err or 'no error logged'}); skipped")
        failed.append(app_id)
        continue
    screenshot(f"{out}/{app_id.lower()}.png")

apps("luapp.admin", {"action": "stop"})
apps("app.show", {"app": "muse"})
print(f"Done. Serial log: {logpath}")
if failed:
    sys.exit(f"no screenshot for: {', '.join(failed)}")
EOF
