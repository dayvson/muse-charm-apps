#!/usr/bin/env bash
# Why don't Muse's commands get answered? Over USB:
#  1. prints the board's status (Wi-Fi, Muse, link),
#  2. runs "luapp.admin list" over the serial console (the firmware's own command path),
#  3. then listens for SECONDS while you ask Muse to try a command (e.g. "list my
#     gadget's apps"), and reports whether Muse's command reached the board and
#     what the link did. Full log in logs/linkcheck-<ts>.log (tokens redacted).
#  Opening the port can restart the board (macOS): fine, it shows a fresh boot.
#
#   scripts/08_link_check.sh [SECONDS=120]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/env.sh"
SECS="${1:-120}"
[ -n "$MUSE_PORT" ] || { echo "No board on USB (set MUSE_PORT)." >&2; exit 1; }
mkdir -p "$ROOT/logs"
"$ROOT/toolchain/tools-venv/bin/python" -I - "$MUSE_PORT" "$SECS" "$ROOT/logs/linkcheck-$(date +%Y%m%d-%H%M%S).log" <<'EOF'
import re, sys, time, serial
port, secs, logpath = sys.argv[1], float(sys.argv[2]), sys.argv[3]
red = lambda t: re.sub(r"(?i)(mgst_\S+|\"?(pass(word)?|psk|token)\"?\s*[=:]\s*\"?[^\s\",}]+)", "<redacted>", t)
ansi = re.compile(r"\x1b\[[0-9;]*m")
s = serial.Serial(); s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dtr = s.rts = False                      # don't reset on open
s.open()
try:                                       # don't reset on close either
    import termios
    a = termios.tcgetattr(s.fd); a[2] &= ~termios.HUPCL; termios.tcsetattr(s.fd, termios.TCSANOW, a)
except (ImportError, AttributeError, OSError):
    pass
log = open(logpath, "w", encoding="utf-8", errors="replace")
pending, ALL = "", []
def lines(secs, until=None):
    global pending
    end, got = time.time() + secs, []
    while time.time() < end:
        pending += s.read(4096).decode("utf-8", "replace")
        *done, pending = pending.split("\n")
        for l in done:
            l = red(ansi.sub("", l.rstrip("\r")))
            log.write(l + "\n"); log.flush()
            got.append(l); ALL.append(l)
            if until and l.startswith(until):
                return got
    return got
def console(cmd, tag, secs=6):
    lines(0.3)
    s.write((">" + cmd + "\n").encode())
    for l in lines(secs, tag):
        if l.startswith(tag):
            return l[len(tag):].strip()
    return None

print("1. Status")
st = console("status", "@status ")
print("   ", st[:600] if st else "no answer from the serial console")

print("\n2. luapp.admin list over USB (the firmware's side of a command)")
r = console('apps luapp.admin {"action":"list"}', "@apps ")
print("   ", (r[:400] if r else "NO ANSWER: the apps/Lua side is stuck"))
w = console("apps watch.get", "@apps ")
print("    watch.get (no Lua involved):", (w[:160] if w else "NO ANSWER"))

print(f"\n3. Now ask Muse to run a gadget command (e.g. 'list my gadget's apps').")
print(f"   Listening {int(secs)} s...\n")
KEY = re.compile(r"invoke request|link\.register|held for DMA|sent link|hb t=|tunnel|reconnect|session|result queue|stale|rejected|"
                 r"keepalive|nothing received|TLS|ws_|WS |luapp|apps_net|E \(|W \(")
invokes, seen = [], []
end = time.time() + secs
while time.time() < end:
    for l in lines(1):
        if KEY.search(l):
            print("   " + l[:220]); seen.append(l)
        if "invoke request" in l:
            invokes.append(l.split("command=")[-1])
print("\nSummary")
if invokes:
    print(f"   Muse's commands reached the board: {', '.join(invokes)}")
    print("   (so the link delivers them; if Muse still timed out, the answer isn't getting back)")
else:
    print("   No command from Muse reached the board in that time: the link isn't delivering them.")
hb = [l for l in seen if "hb t=" in l]
if hb:
    m = re.search(r"dma=(\d+)K", hb[-1])
    if m: print(f"   free DMA RAM at the end: {m.group(1)} KB (the link sends only with 16 KB or more free)")
if not any("sent link.register" in l for l in ALL):
    print("   link.register was not sent while listening (fine if the board had been up a while; bad right after a boot)")
for k in ("reconnect", "rejected", "result queue full", "stale control session", "tunnel stream open failed"):
    n = sum(k in l for l in seen)
    if n: print(f"   '{k}': {n}x")
print(f"\nSaved: {logpath}")
EOF
