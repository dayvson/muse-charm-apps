#!/usr/bin/env python3
"""Preview the stock tracker's layout from real responses (tests/fixtures).

Mirrors firmware/stocks_ui.c's geometry and colours with the board's own
Montserrat, so the design can be judged before flashing.

Usage: preview_stocks.py FIXTURES_DIR OUT_DIR
"""
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W = 466
C = W // 2
MONT = Path(__file__).resolve().parents[2] / "muse-gadget-sdk/esp32/components/muse/eink/fonts/montserrat.ttf"

# geometry (keep in step with stocks_ui.c)
PILL_Y, PILL_W, PILL_H, PILL_GAP = 92, 100, 48, 8
PRICE_Y = 158
CHANGE_Y = 204
CHART_X, CHART_Y, CHART_W, CHART_H = 53, 232, 360, 124
CHIP_Y, CHIP_W, CHIP_H, CHIP_GAP = 388, 54, 30, 7
FOOT_Y = 428

GREEN = (48, 209, 88)
RED = (255, 69, 58)
TEXT = (255, 255, 255)
DIM = (142, 142, 147)
PILL_BG = (28, 28, 30)
PILL_LINE = (58, 58, 60)
BASE_LINE = (72, 72, 74)

RANGES = [("1D", "1d", "Today"), ("1W", "5d", "Past week"), ("1M", "1mo", "Past month"),
          ("1Y", "1y", "Past year"), ("5Y", "5y", "Past 5 years")]
SYMS = ["META", "SHOP", "SPY"]


def font(px):
    return ImageFont.truetype(str(MONT), px)


def load(fx, sym, rng):
    r = json.load(open(fx / f"{sym}_{rng}.json"))["chart"]["result"][0]
    m = r["meta"]
    pts = [(t, c) for t, c in zip(r["timestamp"], r["indicators"]["quote"][0]["close"]) if c is not None]
    return m, pts


def text(d, xy, s, f, fill, anchor="mm"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def fmt_change(now, base):
    dv = now - base
    pct = dv / base * 100
    return f"{'+' if dv >= 0 else '-'}{abs(dv):.2f} ({'+' if pct >= 0 else '-'}{abs(pct):.2f}%)"


def chart(img, m, pts, rng, col):
    S = 4
    cw, ch = CHART_W * S, CHART_H * S
    layer = Image.new("RGBA", (cw, ch), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    vals = [c for _, c in pts]
    base = m["chartPreviousClose"]
    lo, hi = min(vals + [base]), max(vals + [base])
    pad = (hi - lo) * 0.08 or 1
    lo, hi = lo - pad, hi + pad
    if rng == "1d":
        t0 = m["currentTradingPeriod"]["regular"]["start"]
        t1 = m["currentTradingPeriod"]["regular"]["end"]
        xs = [(t - t0) / (t1 - t0) for t, _ in pts]
    else:
        xs = [i / (len(pts) - 1) for i in range(len(pts))]
    P = [(x * (cw - 1), (hi - c) / (hi - lo) * (ch - 1)) for x, c in zip(xs, vals)]
    # fill: colour fading to nothing at the bottom
    for i in range(len(P) - 1):
        (x0, y0), (x1, y1) = P[i], P[i + 1]
        for x in range(int(x0), int(x1) + 1):
            t = (x - x0) / (x1 - x0) if x1 > x0 else 0
            y = y0 + (y1 - y0) * t
            for yy in range(int(y), ch, S):
                a = int(70 * (1 - (yy - y) / (ch - y + 1)))
                d.line([(x, yy), (x, yy + S)], fill=col + (max(a, 0),))
    # previous close / range base: dotted
    by = (hi - base) / (hi - lo) * (ch - 1)
    for x in range(0, cw, 6 * S):
        d.line([(x, by), (x + 2 * S, by)], fill=BASE_LINE + (255,), width=S)
    d.line(P, fill=col + (255,), width=int(2.5 * S), joint="curve")
    lx, ly = P[-1]
    d.ellipse([lx - 4 * S, ly - 4 * S, lx + 4 * S, ly + 4 * S], fill=col + (255,))
    layer = layer.resize((CHART_W, CHART_H), Image.LANCZOS)
    img.alpha_composite(layer, (CHART_X, CHART_Y))


def render(fx, sel, rng_i):
    img = Image.new("RGBA", (W, W), (0, 0, 0, 255))
    d = ImageDraw.Draw(img)
    f48, f20, f16, f14 = font(48), font(20), font(16), font(14)
    # pills: ticker + today's change
    for i, s in enumerate(SYMS):
        m, _ = load(fx, s, "1d")
        day = (m["regularMarketPrice"] - m["previousClose"]) / m["previousClose"] * 100
        col = GREEN if day >= 0 else RED
        cx = C + (i - 1) * (PILL_W + PILL_GAP)
        box = [cx - PILL_W / 2, PILL_Y - PILL_H / 2, cx + PILL_W / 2, PILL_Y + PILL_H / 2]
        if s == sel:
            d.rounded_rectangle(box, radius=PILL_H / 2, fill=PILL_BG, outline=col, width=2)
        else:
            d.rounded_rectangle(box, radius=PILL_H / 2, outline=PILL_LINE, width=1)
        text(d, (cx, PILL_Y - 9), s, f16, TEXT if s == sel else DIM)
        text(d, (cx, PILL_Y + 11), f"{'+' if day >= 0 else ''}{day:.2f}%", f14, col)
    label, rng, period = RANGES[rng_i]
    m, pts = load(fx, sel, rng)
    up = m["regularMarketPrice"] >= m["chartPreviousClose"]
    col = GREEN if up else RED
    text(d, (C, PRICE_Y), f"${m['regularMarketPrice']:,.2f}", f48, TEXT)
    ch = fmt_change(m["regularMarketPrice"], m["chartPreviousClose"])
    w1 = d.textlength(ch, font=f20)
    w2 = d.textlength(period, font=f14)
    x = C - (w1 + 8 + w2) / 2
    text(d, (x, CHANGE_Y), ch, f20, col, anchor="lm")
    text(d, (x + w1 + 8, CHANGE_Y + 1), period, f14, DIM, anchor="lm")
    chart(img, m, pts, rng, col)
    for i, (lb, _, _) in enumerate(RANGES):
        cx = C + (i - 2) * (CHIP_W + CHIP_GAP)
        box = [cx - CHIP_W / 2, CHIP_Y - CHIP_H / 2, cx + CHIP_W / 2, CHIP_Y + CHIP_H / 2]
        if i == rng_i:
            d.rounded_rectangle(box, radius=CHIP_H / 2, fill=col)
            text(d, (cx, CHIP_Y), lb, f16, (0, 0, 0))
        else:
            text(d, (cx, CHIP_Y), lb, f16, DIM)
    text(d, (C, FOOT_Y), "At close • FRI 4:00 PM ET", f14, DIM)
    mask = Image.new("L", (W, W), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, W - 1, W - 1], fill=255)
    out = Image.new("RGB", (W, W), (40, 40, 40))
    out.paste(img.convert("RGB"), (0, 0), mask)
    return out


def main():
    fx, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    for sel, r in (("META", 0), ("SHOP", 3), ("SPY", 4), ("META", 1)):
        render(fx, sel, r).save(out / f"stocks-{sel}-{RANGES[r][0]}.png")
    print(f"previews in {out}")


if __name__ == "__main__":
    main()
