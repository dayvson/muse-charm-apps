#!/usr/bin/env python3
"""Check firmware/wind_data.c's parse of real Open-Meteo responses against Python's json."""
import json, subprocess, sys, tempfile
from datetime import datetime, timezone, timedelta
from pathlib import Path
HERE = Path(__file__).resolve().parent
FW = HERE.parent
files = sorted((HERE / "fixtures").glob("wind_*.json"))
if not files:
    print("no wind fixtures: run tests/fetch_fixtures.sh"); sys.exit(1)
exe = Path(tempfile.mkdtemp()) / "wind_cli"
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-fsanitize=address,undefined", f"-I{FW}",
                str(HERE / "wind_cli.c"), str(FW / "wind_data.c"), str(FW / "apps_json.c"), str(FW / "muse_tz.c"),
                "-lm", "-o", str(exe)], check=True)
out = subprocess.run([str(exe)] + [str(f) for f in files], capture_output=True, text=True)
sys.stderr.write(out.stderr)
fail = out.returncode != 0
for f, line in zip(files, out.stdout.splitlines()):
    got = dict(kv.split("=") for kv in line.split()[1:])
    d = json.load(open(f))
    c, h, dy = d["current"], d["hourly"], d["daily"]
    off = d["utc_offset_seconds"]
    tz = timezone(timedelta(seconds=off))
    tm = [t for t in h["time"] if dy["time"][1] <= t < dy["time"][1] + 86400 and 6 <= datetime.fromtimestamp(t, tz).hour <= 21]
    want = {"ok": "1", "off": str(off), "now": f"{c['wind_speed_10m']:.1f}", "gust": f"{c['wind_gusts_10m']:.1f}",
            "dir": str(c["wind_direction_10m"]), "nh": str(len(h["time"])), "nd": str(len(dy["time"])),
            "h0s": f"{h['wind_speed_10m'][0]:.1f}", "hlast": f"{h['wind_speed_10m'][-1]:.1f}",
            "d0": f"{dy['wind_speed_10m_max'][0]:.1f}", "dlast": f"{dy['wind_speed_10m_max'][-1]:.1f}",
            "g0": f"{dy['wind_gusts_10m_max'][0]:.1f}", "dd0": str(dy["wind_direction_10m_dominant"][0]),
            "tmrw_hours": str(len(tm)), "tmrw_first": "6",
            "wd0": datetime.fromtimestamp(dy["time"][0], tz).strftime("%a").upper()}
    bad = {k: (v, got.get(k)) for k, v in want.items() if got.get(k) != v}
    print(f"  {f.name}: tomorrow's best window {got['best']}, best day {got['bestday']}")
    if bad:
        fail = True
        print(f"MISMATCH {f.name}: {bad}")
print(f"{len(files)} real forecasts: {'FAIL' if fail else 'OK'}")
sys.exit(1 if fail else 0)
