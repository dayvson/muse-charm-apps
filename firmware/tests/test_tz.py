#!/usr/bin/env python3
"""Check firmware/muse_tz.c against Python's zoneinfo for every built-in zone.

Samples every 6 hours from 2026-11-21 to 2036 plus one second either side of every
offset change, and compares offset, DST flag and the local calendar time.
Also checks that malformed rules are refused and zone lookup works.

Usage: test_tz.py  (compiles tz_cli with the host cc into a temp dir)
"""
import re
import subprocess
import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

HERE = Path(__file__).resolve().parent
FW = HERE.parent

zones = re.findall(r'\{"([^"]+)", "([^"]+)", "[^"]+", "([^"]+)"\}', (FW / "muse_zones.c").read_text())
assert len(zones) > 50, len(zones)

tmp = Path(tempfile.mkdtemp())
exe = tmp / "tz_cli"
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-fsanitize=address,undefined",
                f"-I{FW}", str(HERE / "tz_cli.c"), str(FW / "muse_tz.c"), str(FW / "muse_zones.c"),
                str(FW / "muse_zones_find.c"), "-o", str(exe)], check=True)

start = datetime(2026, 11, 21, tzinfo=timezone.utc)   # rules apply from today on (BC went to permanent UTC-7 on 2026-11-01)
end = datetime(2036, 12, 31, tzinfo=timezone.utc)
lines, expect = [], []
for code, iana, posix in zones:
    zi = ZoneInfo(iana)
    t, prev, samples = start, None, set()
    while t < end:
        off = zi.utcoffset(t)
        if prev is not None and off != prev:
            # bisect the change to the second
            lo, hi = t - timedelta(hours=6), t
            while (hi - lo).total_seconds() > 1:
                mid = lo + (hi - lo) / 2
                mid = mid.replace(microsecond=0)
                if zi.utcoffset(mid) == prev:
                    lo = mid
                else:
                    hi = mid
            samples.update({int(lo.timestamp()), int(hi.timestamp()), int(hi.timestamp()) + 1})
        samples.add(int(t.timestamp()))
        prev = off
        t += timedelta(hours=6)
    for u in sorted(samples):
        dt = datetime.fromtimestamp(u, zi)
        off = int(dt.utcoffset().total_seconds())
        dst = int(bool(dt.dst()))
        lines.append(f"{posix} {u}")
        expect.append((code, u, f"{off} {dst} {dt:%Y-%m-%d %H:%M:%S} {int(dt.strftime('%w'))}"))

bad = ["", "EST", "E5", "EST5EDT,M13.1.0,M11.1.0", "EST5EDT,M3.2.0", "EST5EDT,M3.6.0,M11.1.0",
       "EST25", "<+0>3", "EST5EDT,M3.2.0,M11.1.0junk", "EST5EDT,J366,J1", "A" * 70 + "5"]
for b in bad:
    lines.append(f"{b or '-'} 0")
out = subprocess.run([str(exe)], input="\n".join(lines) + "\n", capture_output=True, text=True, check=True).stdout.splitlines()
assert len(out) == len(lines), (len(out), len(lines))

fail = 0
for (code, u, want), got in zip(expect, out):
    if got != want:
        fail += 1
        if fail <= 10:
            print(f"MISMATCH {code} {u}: want {want!r} got {got!r}")
for b, got in zip(bad, out[len(expect):]):
    if got != "ERR":
        fail += 1
        print(f"ACCEPTED bad rule {b!r}: {got}")

found = subprocess.run([str(exe), "find", "tlv", "Tel Aviv", "asia/jerusalem", "MPK", "nowhere"],
                       capture_output=True, text=True, check=True).stdout
want_found = "tlv -> TLV\nTel Aviv -> TLV\nasia/jerusalem -> TLV\nMPK -> MPK\nnowhere -> (none)\n"
if found != want_found:
    fail += 1
    print("zone lookup:", found)

print(f"{len(expect)} samples over {len(zones)} zones, {len(bad)} bad rules: {'FAIL ' + str(fail) if fail else 'OK'}")
sys.exit(1 if fail else 0)
