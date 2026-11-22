#!/usr/bin/env python3
"""Frame-by-frame comparison of make_gifs output against the source GIFs (same names).

    compare.py SRC_GIF_DIR PREVIEW_DIR [state ...]

Prints mean absolute error per channel (0-255) and the share of cells that differ
(sampled at each 5x5 cell's centre)."""
import os
import sys

from PIL import Image, ImageChops, ImageStat

src, prev = sys.argv[1], sys.argv[2]
states = sys.argv[3:] or ["boot", "listening", "thinking", "working", "speaking", "error", "off", "idle", "happy"]
for s in states:
    a, b = os.path.join(src, s + ".gif"), os.path.join(prev, s + ".gif")
    if not (os.path.exists(a) and os.path.exists(b)):
        continue
    A, B = Image.open(a), Image.open(b)
    n = min(A.n_frames, B.n_frames)
    mae, cells_diff = 0.0, 0
    for i in range(n):
        A.seek(i)
        B.seek(i)
        fa, fb = A.convert("RGB"), B.convert("RGB")
        mae += sum(ImageStat.Stat(ImageChops.difference(fa, fb)).mean) / 3
        ca = fa.resize((64, 64), Image.Resampling.NEAREST)
        cb = fb.resize((64, 64), Image.Resampling.NEAREST)
        d = ImageChops.difference(ca, cb).convert("L").point(lambda v: 255 if v > 24 else 0)
        cells_diff += d.histogram()[255]
    print(f"{s:10s} {n:4d} frames  MAE {mae / n:6.2f}  cells differing {100 * cells_diff / (n * 4096):5.2f}%")
