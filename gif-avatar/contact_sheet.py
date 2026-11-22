#!/usr/bin/env python3
"""Contact sheet of make_gifs output: N evenly spaced frames per state.

    contact_sheet.py PREVIEW_DIR OUT.png [--frames 4] [--size 200] [--title TEXT]"""
import argparse
import os

from PIL import Image, ImageDraw

STATES = ["boot", "idle", "listening", "thinking", "working", "speaking", "happy", "off", "error"]
ap = argparse.ArgumentParser()
ap.add_argument("dir")
ap.add_argument("out")
ap.add_argument("--frames", type=int, default=4)
ap.add_argument("--size", type=int, default=200)
ap.add_argument("--title", default="")
a = ap.parse_args()
rows = [s for s in STATES if os.path.exists(os.path.join(a.dir, s + ".gif"))]
lab, top = 18, 24 if a.title else 0
sheet = Image.new("RGB", (a.frames * a.size, top + len(rows) * (a.size + lab)), (20, 20, 24))
d = ImageDraw.Draw(sheet)
if a.title:
    d.text((6, 6), a.title, fill=(255, 255, 255))
for r, s in enumerate(rows):
    g = Image.open(os.path.join(a.dir, s + ".gif"))
    n = g.n_frames
    y = top + r * (a.size + lab)
    d.text((4, y + 3), f"{s}  ({n} distinct frames)", fill=(220, 220, 220))
    for c in range(a.frames):
        i = min(n - 1, int((c + 0.5) * n / a.frames))
        g.seek(i)
        sheet.paste(g.convert("RGB").resize((a.size, a.size), Image.Resampling.NEAREST), (c * a.size, y + lab))
        d.text((c * a.size + 4, y + lab + 3), f"f{i}", fill=(150, 150, 150))
sheet.save(a.out)
print(a.out, sheet.size)
