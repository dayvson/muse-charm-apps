#!/usr/bin/env python3
"""Check firmware/stocks_data.c's parse of real Yahoo responses against Python's json."""
import json, subprocess, sys, tempfile
from pathlib import Path
HERE = Path(__file__).resolve().parent
FW = HERE.parent
files = sorted(f for f in (HERE / "fixtures").glob("*.json") if not f.name.startswith("wind_"))
if not files:
    print("no fixtures: run tests/fetch_fixtures.sh"); sys.exit(1)
exe = Path(tempfile.mkdtemp()) / "stocks_cli"
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-fsanitize=address,undefined", f"-I{FW}",
                str(HERE / "stocks_cli.c"), str(FW / "stocks_data.c"), str(FW / "apps_json.c"), "-lm", "-o", str(exe)], check=True)
out = subprocess.run([str(exe)] + [str(f) for f in files], capture_output=True, text=True)
sys.stderr.write(out.stderr)
fail = out.returncode != 0
for f, line in zip(files, out.stdout.splitlines()):
    got = dict(kv.split("=") for kv in line.split()[1:])
    r = json.load(open(f))["chart"]["result"][0]
    m = r["meta"]
    pts = [(t, c) for t, c in zip(r["timestamp"], r["indicators"]["quote"][0]["close"]) if c is not None and t is not None]
    want = {"ok": "1", "price": f"{m['regularMarketPrice']:.2f}", "base": f"{m['chartPreviousClose']:.2f}",
            "n": str(min(len(pts), 280)), "last": f"{pts[-1][1]:.2f}", "tn": str(pts[-1][0]),
            "first": f"{pts[0][1]:.2f}", "t0": str(pts[0][0]),
            "ss": str(m["currentTradingPeriod"]["regular"]["start"]), "se": str(m["currentTradingPeriod"]["regular"]["end"])}
    bad = {k: (v, got.get(k)) for k, v in want.items() if got.get(k) != v}
    if bad:
        fail = True
        print(f"MISMATCH {f.name}: {bad}")
print(f"{len(files)} real responses: {'FAIL' if fail else 'OK'}")
sys.exit(1 if fail else 0)
