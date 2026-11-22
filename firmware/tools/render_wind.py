#!/usr/bin/env python3
"""The wind tracker's icons (-> firmware/wind_assets.c) and layout previews.

Previews mirror firmware/wind_ui.c's geometry and colours with the board's
Montserrat, from real Open-Meteo responses in tests/fixtures.

Usage: render_wind.py FIRMWARE_DIR PREVIEW_DIR FIXTURES_DIR
"""
import json
import math
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

S = 4
W = 466
C = W // 2
MONT = Path(__file__).resolve().parents[2] / "muse-gadget-sdk/esp32/components/muse/eink/fonts/montserrat.ttf"

# geometry (keep in step with wind_ui.c)
NAME_Y, DOTS_Y = 62, 86
RING_X, RING_Y, RING_R = 160, 140, 44
NUM_X, NUM_Y, SUB_Y = 218, 128, 176
BADGE_Y = 212
CHART_X, CHART_Y, CHART_W, CHART_H = 53, 236, 360, 120
CHIP_Y, CHIP_W, CHIP_H, CHIP_GAP = 390, 92, 30, 8
FOOT_Y = 430
BAR_BASE, BAR_TOP = 84, 20       # chart rows (canvas coordinates)
ARROW_Y, LABEL_Y = 95, 111

TEAL = (39, 211, 162)
DIM = (142, 142, 147)
RING = (30, 47, 58)


def color(kn):
    for lim, c in ((6, 0x4a6b8a), (10, 0x2eb8e6), (14, 0x27d3a2), (18, 0x7be04a), (22, 0xf5d547), (27, 0xff9f2e),
                   (33, 0xff4d4d)):
        if kn < lim:
            return ((c >> 16) & 255, (c >> 8) & 255, c & 255)
    return (0xd3, 0x5c, 0xff)


def rate(kn):
    if kn < 11: return "TOO LIGHT", (0x4a, 0x6b, 0x8a)
    if kn < 14: return "MARGINAL", (0x27, 0xd3, 0xa2)
    if kn < 25: return "GO KITE!", (0x7b, 0xe0, 0x4a)
    if kn < 32: return "HOLD ON!", (0xff, 0x9f, 0x2e)
    return "NUKING", (0xd3, 0x5c, 0xff)


def compass(deg):
    P = "N NNE NE ENE E ESE SE SSE S SSW SW WSW W WNW NW NNW".split()
    return P[((deg % 360) * 16 + 180) // 360 % 16]


# ---------------------------------------------------------------- icons

def kite_icon():
    """A diamond kite with a bow-tied tail, in black (it sits on the coloured badge)."""
    w = 24
    img = Image.new("RGBA", (w * S, w * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    k = [(12, 1), (19, 8), (12, 15), (5, 8)]
    d.polygon([(x * S, y * S) for x, y in k], fill=(0, 0, 0, 255))
    d.line([(12 * S, 2 * S), (12 * S, 14 * S)], fill=(0, 0, 0, 0), width=int(1.2 * S))   # spar
    d.line([(6 * S, 8 * S), (18 * S, 8 * S)], fill=(0, 0, 0, 0), width=int(1.2 * S))
    tail = [(12, 15), (11, 18), (14, 20), (12, 23)]
    d.line([(x * S, y * S) for x, y in tail], fill=(0, 0, 0, 255), width=int(1.4 * S), joint="curve")
    for bx, by in ((11.5, 18.2), (13.6, 20.4)):
        d.polygon([((bx - 2) * S, (by - 1) * S), ((bx + 2) * S, (by + 1) * S), ((bx + 2) * S, (by - 1) * S),
                   ((bx - 2) * S, (by + 1) * S)], fill=(0, 0, 0, 255))
    return img.resize((w, w), Image.LANCZOS)


def arrow_icon():
    """The current wind's arrow, pointing up (LVGL turns it downwind), white."""
    w = 60
    img = Image.new("RGBA", (w * S, w * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = w / 2
    pts = [(c, 6), (c + 15, 30), (c + 5.5, 26), (c + 5.5, 54), (c - 5.5, 54), (c - 5.5, 26), (c - 15, 30)]
    d.polygon([(x * S, y * S) for x, y in pts], fill=(255, 255, 255, 255))
    return img.resize((w, w), Image.LANCZOS)


def argb(img, bleed):
    out = bytearray()
    for r, g, b, a in img.get_flattened_data():
        if a == 0:
            r, g, b = bleed
        out += bytes((b, g, r, a))
    return bytes(out)


def c_array(name, data):
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 24]) for i in range(0, len(data), 24)]
    return f"static const uint8_t {name}[{len(data)}] = {{\n    " + ",\n    ".join(rows) + "\n};\n"


# ---------------------------------------------------------------- preview

def font(px):
    return ImageFont.truetype(str(MONT), px)


def text(d, xy, s, f, fill, anchor="mm"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def small_arrow(d, x, y, deg, col):
    """A triangle pointing downwind (deg = where the wind comes from)."""
    a = math.radians(deg + 180)
    ux, uy = math.sin(a), -math.cos(a)
    px, py = -uy, ux
    tip = (x + ux * 6, y + uy * 6)
    l = (x - ux * 5 + px * 4.5, y - uy * 5 + py * 4.5)
    r = (x - ux * 5 - px * 4.5, y - uy * 5 - py * 4.5)
    d.polygon([tip, l, r], fill=col)


def chart(img, f, view, now):
    cv = Image.new("RGBA", (CHART_W, CHART_H), (0, 0, 0, 255))
    d = ImageDraw.Draw(cv)
    f14 = font(14)
    tz = timezone(timedelta(seconds=f["utc_offset_seconds"]))
    h, dy = f["hourly"], f["daily"]
    if view < 2:
        d0 = dy["time"][view]
        idx = [i for i, t in enumerate(h["time"]) if d0 <= t < d0 + 86400 and 6 <= datetime.fromtimestamp(t, tz).hour <= 21]
        sp = [h["wind_speed_10m"][i] for i in idx]
        gu = [h["wind_gusts_10m"][i] for i in idx]
        di = [h["wind_direction_10m"][i] for i in idx]
        hours = [datetime.fromtimestamp(h["time"][i], tz).hour for i in idx]
        past = [h["time"][i] + 3600 <= now for i in idx] if view == 0 else [False] * len(idx)
        labels = [(k, f"{(hr % 12) or 12}{'a' if hr < 12 else 'p'}") for k, hr in enumerate(hours) if hr % 3 == 0]
        numbers = [k for k, hr in enumerate(hours) if hr % 2 == 0]
        bw = 14
    else:
        n = len(dy["time"])
        sp, gu, di = dy["wind_speed_10m_max"], dy["wind_gusts_10m_max"], dy["wind_direction_10m_dominant"]
        past = [False] * n
        labels = [(k, datetime.fromtimestamp(t, tz).strftime("%a").upper()) for k, t in enumerate(dy["time"])]
        numbers = list(range(n))
        bw = 26
    n = len(sp)
    slot = CHART_W / n
    top = max(25, max(gu))
    for k in range(n):
        cx = slot * (k + 0.5)
        col = color(sp[k])
        if past[k]:
            col = tuple(int(c * 0.4) for c in col)
        y = BAR_BASE - sp[k] / top * (BAR_BASE - BAR_TOP)
        gy = BAR_BASE - gu[k] / top * (BAR_BASE - BAR_TOP)
        ghost = tuple(int(c * 0.32) for c in col)        # gusts: the bar's colour, faint, above it
        d.rounded_rectangle([cx - bw / 2, gy, cx + bw / 2, BAR_BASE], radius=3, fill=ghost)
        d.rounded_rectangle([cx - bw / 2, y, cx + bw / 2, BAR_BASE], radius=3, fill=col)
        if k in numbers:
            text(d, (cx, min(y, gy) - 9), f"{round(sp[k])}", f14, (255, 255, 255) if not past[k] else (100, 100, 100))
            small_arrow(d, cx, ARROW_Y, di[k], (200, 200, 200) if not past[k] else (80, 80, 80))
    for k, lb in labels:
        text(d, (slot * (k + 0.5), LABEL_Y), lb, f14, DIM)
    img.paste(cv, (CHART_X, CHART_Y))


def render(fx, name, view, kite, arrow):
    f = json.load(open(fx))
    img = Image.new("RGBA", (W, W), (0, 0, 0, 255))
    d = ImageDraw.Draw(img)
    f48, f20, f16, f14 = font(48), font(20), font(16), font(14)
    text(d, (C, NAME_Y), name, f20, (255, 255, 255))
    for i in range(3):
        x = C + (i - 1) * 16
        d.ellipse([x - 4, DOTS_Y - 4, x + 4, DOTS_Y + 4], fill=TEAL if i == (0 if "Crandon" in name else 1) else (58, 58, 60))
    c = f["current"]
    kn, gust, deg = c["wind_speed_10m"], c["wind_gusts_10m"], c["wind_direction_10m"]
    d.ellipse([RING_X - RING_R, RING_Y - RING_R, RING_X + RING_R, RING_Y + RING_R], outline=RING, width=3)
    for t in range(0, 360, 90):
        a = math.radians(t)
        d.line([(RING_X + math.sin(a) * (RING_R - 7), RING_Y - math.cos(a) * (RING_R - 7)),
                (RING_X + math.sin(a) * (RING_R - 2), RING_Y - math.cos(a) * (RING_R - 2))], fill=(70, 90, 100), width=2)
    rot = arrow.rotate(-(deg + 180), resample=Image.BICUBIC)
    img.alpha_composite(rot, (RING_X - 30, RING_Y - 30))
    num = f"{round(kn)}"
    text(d, (NUM_X, NUM_Y), num, f48, color(kn), anchor="lm")
    wn = d.textlength(num, font=f48)
    text(d, (NUM_X + wn + 6, NUM_Y + 10), "kn", f20, DIM, anchor="lm")
    text(d, (NUM_X, SUB_Y), f"gust {round(gust)} • {compass(deg)}", f16, DIM, anchor="lm")
    label, rc = rate(kn)
    tw = d.textlength(label, font=f16)
    bw = tw + 24 + 6 + 28
    d.rounded_rectangle([C - bw / 2, BADGE_Y - 15, C + bw / 2, BADGE_Y + 15], radius=15, fill=rc)
    img.alpha_composite(kite, (int(C - bw / 2 + 12), BADGE_Y - 12))
    text(d, (C - bw / 2 + 12 + 24 + 6, BADGE_Y), label, f16, (0, 0, 0), anchor="lm")
    chart(img, f, view, c["time"])
    for i, lb in enumerate(("TODAY", "TMRW", "WEEK")):
        cx = C + (i - 1) * (CHIP_W + CHIP_GAP)
        box = [cx - CHIP_W / 2, CHIP_Y - CHIP_H / 2, cx + CHIP_W / 2, CHIP_Y + CHIP_H / 2]
        if i == view:
            d.rounded_rectangle(box, radius=CHIP_H / 2, fill=TEAL)
            text(d, (cx, CHIP_Y), lb, f16, (0, 0, 0))
        else:
            text(d, (cx, CHIP_Y), lb, f16, DIM)
    text(d, (C, FOOT_Y), ["Best 1 PM-5 PM • 14-17 kn", "No kiteable wind 7 AM-7 PM", "Best day: SUN • 16 kn"][view], f14, TEAL)
    mask = Image.new("L", (W, W), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, W - 1, W - 1], fill=255)
    out = Image.new("RGB", (W, W), (40, 40, 40))
    out.paste(img.convert("RGB"), (0, 0), mask)
    return out


def main():
    fw, prev, fx = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    prev.mkdir(parents=True, exist_ok=True)
    kite, arrow = kite_icon(), arrow_icon()
    src = ["/* Generated by firmware/tools/render_wind.py. Do not edit. */", '#include "wind_assets.h"', ""]
    for name, img, bleed in (("kite", kite, (0, 0, 0)), ("arrow", arrow, (255, 255, 255))):
        src.append(c_array(f"{name}_px", argb(img, bleed)))
        src.append(f"const watch_image_t wind_{name} = {{ {img.size[0]}, {img.size[1]}, {img.size[0] // 2}, "
                   f"{img.size[1] // 2}, {name}_px }};\n")
    (fw / "wind_assets.c").write_text("\n".join(src))
    for spot, name in (("crandon", "Crandon Park"), ("stiltsville", "Stiltsville")):
        for v, tag in ((0, "today"), (1, "tmrw"), (2, "week")):
            render(fx / f"wind_{spot}.json", name, v, kite, arrow).save(prev / f"wind-{spot}-{tag}.png")
    print(f"assets in {fw / 'wind_assets.c'}, previews in {prev}")


if __name__ == "__main__":
    main()
