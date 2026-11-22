#!/usr/bin/env python3
"""Render the dual-time pocket watch: dial, hands and icons -> firmware/watch_assets.c.

Everything is drawn 4x oversized and scaled down, so edges are smooth. The
dial becomes a 256-colour LVGL I8 image (1 KB palette + one byte a pixel),
which LVGL draws straight from flash a row at a time; hands and icons are
ARGB8888 images LVGL rotates.

Usage: render_watch.py OUT_DIR_FOR_C PREVIEW_DIR
  writes OUT/watch_assets.c and PREVIEW/{dial,watch-*}.png
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

W = 466                 # the 1.75C's round AMOLED
C = W / 2
S = 4                   # supersampling
SUBDIAL = (C, C + 100)  # small seconds at 6 o'clock
SUB_R = 46
WIN_X = 112             # WORK/LOCAL windows' centres, either side of the axis
WIN_W, WIN_H = 106, 38
DATE_Y = C - 90
DATE_W, DATE_H = 84, 30

AVENIR = "/System/Library/Fonts/Avenir Next.ttc"
MONTSERRAT = Path(__file__).resolve().parents[2] / "muse-gadget-sdk/esp32/components/muse/eink/fonts/montserrat.ttf"

# palette
BLACK = (0, 0, 0)
CHAPTER = (7, 9, 13)
DIAL_IN = (16, 33, 56)        # midnight blue, centre
DIAL_OUT = (8, 17, 31)        # towards the edge
GROOVE = 0.74                 # guilloche groove brightness
GOLD = (222, 214, 198)        # white gold
GOLD_DK = (128, 120, 106)
LUME = (238, 244, 233)
IVORY = (232, 226, 210)
COPPER = (236, 138, 92)       # work time
META_BLUE = (0, 100, 224)     # local time: #0064E0
META_BLUE_LT = (92, 160, 255) # its tint for small text on the dark dial
WINDOW = (3, 6, 10)
SHADOW = (2, 5, 10)


def font(size, index=0, path=AVENIR):
    return ImageFont.truetype(str(path), int(size * S), index=index)


def sc(v):
    return v * S


def lerp(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def mul(c, k):
    return tuple(min(255, int(round(x * k))) for x in c)


def polar(r, deg, centre=(C, C)):
    a = math.radians(deg)
    return centre[0] + r * math.sin(a), centre[1] - r * math.cos(a)


def circle(draw, centre, r, **kw):
    x, y = centre
    draw.ellipse([sc(x - r), sc(y - r), sc(x + r), sc(y + r)], **kw)


def ring_mask(r_out, r_in, centre=(C, C)):
    m = Image.new("L", (sc(W), sc(W)), 0)
    d = ImageDraw.Draw(m)
    circle(d, centre, r_out, fill=255)
    if r_in > 0:
        circle(d, centre, r_in, fill=0)
    return m


def text_centered(draw, xy, s, f, fill, spacing=0, stroke=0, stroke_fill=None):
    """Centre `s` on xy (final px), with extra letter spacing (final px)."""
    widths = [f.getlength(ch) for ch in s]
    total = sum(widths) + sc(spacing) * (len(s) - 1)
    asc, desc = f.getmetrics()
    bbox = f.getbbox("0" if s.strip("0123456789") == "" else s)
    h = bbox[3] - bbox[1]
    x = sc(xy[0]) - total / 2
    y = sc(xy[1]) - h / 2 - bbox[1]
    for ch, w in zip(s, widths):
        draw.text((x, y), ch, font=f, fill=fill, stroke_width=stroke, stroke_fill=stroke_fill)
        x += w + sc(spacing)


# ---------------------------------------------------------------- dial

def render_dial():
    img = Image.new("RGB", (sc(W), sc(W)), BLACK)
    d = ImageDraw.Draw(img)

    # Case edge: a polished rim, lit from the upper left.
    grad = Image.linear_gradient("L").rotate(45, resample=Image.BICUBIC, expand=False).resize((sc(W), sc(W)))
    lo, hi = (40, 42, 46), (205, 205, 210)
    rim = Image.merge("RGB", [grad.point(lambda v, a=a, b=b: int(a + (b - a) * (v / 255) ** 1.4)) for a, b in zip(lo, hi)])
    img.paste(rim, (0, 0), ring_mask(233, 227))
    circle(d, (C, C), 227, fill=CHAPTER)

    # Railway minute track.
    for r, w in ((221, 1.3), (204, 1.0)):
        circle(d, (C, C), r, outline=mul(IVORY, 0.45), width=int(sc(w)))
    for m in range(60):
        deg = m * 6
        big = m % 5 == 0
        r0, r1, w = (199, 221, 3.4) if big else (206, 219, 1.5)
        col = IVORY if big else mul(IVORY, 0.75)
        d.line([tuple(map(sc, polar(r0, deg))), tuple(map(sc, polar(r1, deg)))], fill=col, width=int(sc(w)))

    # Inner dial: midnight blue with concentric guilloche grooves.
    dial = Image.new("RGB", (sc(W), sc(W)), BLACK)
    dd = ImageDraw.Draw(dial)
    R = 194
    for r in range(R, 0, -2):
        t = (r / R) ** 1.6
        base = lerp(DIAL_IN, DIAL_OUT, t)
        col = mul(base, GROOVE) if (r // 2) % 2 else base
        circle(dd, (C, C), r, fill=col)
    img.paste(dial, (0, 0), ring_mask(R, 0))
    circle(d, (C, C), R, outline=(44, 60, 84), width=int(sc(1.5)))     # bevel highlight
    circle(d, (C, C), R + 1.5, outline=(2, 3, 5), width=int(sc(1.2)))

    # Small seconds: a recessed, more finely grooved sub-dial.
    sub = Image.new("RGB", (sc(W), sc(W)), BLACK)
    sd = ImageDraw.Draw(sub)
    for r in range(SUB_R, 0, -1):
        base = lerp((10, 20, 34), (5, 11, 20), (r / SUB_R) ** 1.3)
        circle(sd, SUBDIAL, r, fill=mul(base, 0.82) if r % 2 else base)
    img.paste(sub, (0, 0), ring_mask(SUB_R, 0, SUBDIAL))
    circle(d, SUBDIAL, SUB_R, outline=(2, 4, 8), width=int(sc(2.5)))      # recess shadow
    circle(d, SUBDIAL, SUB_R + 1, outline=mul(GOLD, 0.55), width=int(sc(1.2)))
    for s in range(60):
        deg = s * 6
        big = s % 5 == 0
        r0 = SUB_R - (9 if big else 5)
        d.line([tuple(map(sc, polar(r0, deg, SUBDIAL))), tuple(map(sc, polar(SUB_R - 2, deg, SUBDIAL)))],
               fill=IVORY if big else mul(IVORY, 0.55), width=int(sc(2.0 if big else 1.0)))
    fs = font(9.5, 2)
    for s, lbl in ((20, "20"), (40, "40"), (0, "60")):
        x, y = polar(SUB_R - 19, s * 6, SUBDIAL)
        text_centered(d, (x, y), lbl, fs, mul(IVORY, 0.8))

    # Applied numerals and batons, with a soft shadow layer underneath.
    shadow = Image.new("L", (sc(W), sc(W)), 0)
    sh = ImageDraw.Draw(shadow)
    marks = Image.new("RGBA", (sc(W), sc(W)), (0, 0, 0, 0))
    md = ImageDraw.Draw(marks)
    fnum = font(56, 0)
    for hour in (12, 2, 4, 8, 10):
        r = 158 if hour in (12,) else 157
        x, y = polar(r, hour * 30)
        text_centered(sh, (x + 2, y + 3), str(hour), fnum, 255, spacing=-2, stroke=int(sc(2.2)), stroke_fill=255)
        text_centered(md, (x, y), str(hour), fnum, LUME + (255,), spacing=-2, stroke=int(sc(2.2)), stroke_fill=GOLD + (255,))
    for hour in (1, 5, 7, 11):
        deg = hour * 30
        r0, r1, half = 150, 184, 5.5
        def bar(dx=0, dy=0, inset=0.0):
            a = math.radians(deg)
            ux, uy = math.sin(a), -math.cos(a)
            px, py = -uy, ux
            pts = []
            for r, side in ((r0 + inset, 1), (r1 - inset, 1), (r1 - inset, -1), (r0 + inset, -1)):
                hw = (half - inset) * side
                pts.append((sc(C + ux * r + px * hw + dx), sc(C + uy * r + py * hw + dy)))
            return pts
        sh.polygon(bar(2, 3), fill=255)
        md.polygon(bar(), fill=GOLD + (255,))
        md.polygon(bar(inset=2.0), fill=LUME + (255,))
    shadow = shadow.filter(ImageFilter.GaussianBlur(sc(1.6)))
    img.paste(Image.new("RGB", img.size, SHADOW), (0, 0), shadow.point(lambda v: int(v * 0.85)))
    img.paste(marks, (0, 0), marks)

    # Windows: WORK at 3, LOCAL at 9, the date under 12.
    def window(cx, cy, w, h, frame):
        box = [sc(cx - w / 2), sc(cy - h / 2), sc(cx + w / 2), sc(cy + h / 2)]
        sh2 = Image.new("L", img.size, 0)
        ImageDraw.Draw(sh2).rounded_rectangle([b + sc(2.5) for b in box], radius=sc(h / 2), fill=200)
        img.paste(Image.new("RGB", img.size, SHADOW), (0, 0), sh2.filter(ImageFilter.GaussianBlur(sc(2))))
        d.rounded_rectangle(box, radius=sc(h / 2), fill=frame)
        inner = [box[0] + sc(2.2), box[1] + sc(2.2), box[2] - sc(2.2), box[3] - sc(2.2)]
        d.rounded_rectangle(inner, radius=sc(h / 2 - 2.2), fill=WINDOW)
        # inner shadow along the top edge
        d.arc([inner[0], inner[1], inner[0] + sc(h - 4.4), inner[3]], 180, 270, fill=(0, 0, 0), width=int(sc(1.5)))
    window(C + WIN_X, C, WIN_W, WIN_H, COPPER)
    window(C - WIN_X, C, WIN_W, WIN_H, META_BLUE)
    window(C, DATE_Y, DATE_W, DATE_H, GOLD)

    flab = font(11, 2)
    text_centered(d, (C + WIN_X, C - 33), "WORK", flab, COPPER, spacing=2.4)
    text_centered(d, (C - WIN_X, C - 33), "LOCAL", flab, META_BLUE_LT, spacing=2.4)

    # Maker's line.
    text_centered(d, (C, C - 50), "MUSE", font(14, 2), mul(IVORY, 0.9), spacing=4.5)
    text_centered(d, (C, C + 38), "DUAL TIME", font(8.5, 2), mul(IVORY, 0.55), spacing=2.6)

    return img.resize((W, W), Image.LANCZOS)


# ---------------------------------------------------------------- hands

def hand_canvas(w, length, tail):
    """A w x (length+tail) RGBA canvas pointing up; pivot at (w/2, length)."""
    return Image.new("RGBA", (sc(w), sc(length + tail)), (0, 0, 0, 0))


def finish(img):
    w, h = img.size[0] // S, img.size[1] // S
    return img.resize((w, h), Image.LANCZOS)


def sword(w, length, tail, max_half, tip, frame, fill, tail_half=3.0, inset=2.2):
    img = hand_canvas(w, length, tail)
    d = ImageDraw.Draw(img)
    cx = w / 2
    def shape(ins):
        y0 = length                      # pivot row
        pts = [
            (cx - (max_half * 0.62 - ins), y0 - 14),
            (cx - (max_half - ins), y0 - length * 0.42),
            (cx, tip + ins * 2.2),
            (cx + (max_half - ins), y0 - length * 0.42),
            (cx + (max_half * 0.62 - ins), y0 - 14),
        ]
        return [(sc(x), sc(y)) for x, y in pts]
    # shaft from the pivot to the blade
    d.rounded_rectangle([sc(cx - tail_half), sc(length - 18), sc(cx + tail_half), sc(length + tail)],
                        radius=sc(tail_half), fill=frame)
    d.polygon(shape(0), fill=frame)
    if fill:
        d.polygon(shape(inset), fill=fill)
    circle(d, (cx, length), max_half * 0.75, fill=frame)
    return img


def skeleton(w, length, tail, max_half, tip, col, stroke=2.6):
    img = hand_canvas(w, length, tail)
    d = ImageDraw.Draw(img)
    cx = w / 2
    y0 = length
    outline = [
        (cx - max_half * 0.55, y0 - 16),
        (cx - max_half, y0 - length * 0.48),
        (cx - max_half * 0.78, y0 - length * 0.70),
        (cx - max_half * 1.35, y0 - length * 0.70),   # arrowhead
        (cx, tip),
        (cx + max_half * 1.35, y0 - length * 0.70),
        (cx + max_half * 0.78, y0 - length * 0.70),
        (cx + max_half, y0 - length * 0.48),
        (cx + max_half * 0.55, y0 - 16),
    ]
    pts = [(sc(x), sc(y)) for x, y in outline]
    d.line(pts + [pts[0]], fill=col, width=int(sc(stroke)), joint="curve")
    d.rounded_rectangle([sc(cx - 2.2), sc(y0 - 17), sc(cx + 2.2), sc(y0 + tail)], radius=sc(2.2), fill=col)
    circle(d, (cx, y0), 7, fill=col)
    return img


def seconds_hand():
    length, tail, w = 38, 12, 14
    img = hand_canvas(w, length, tail)
    d = ImageDraw.Draw(img)
    cx = w / 2
    col = (255, 104, 62)
    d.polygon([(sc(cx - 1.4), sc(length)), (sc(cx), sc(1)), (sc(cx + 1.4), sc(length))], fill=col)
    d.rounded_rectangle([sc(cx - 1.4), sc(length - 2), sc(cx + 1.4), sc(length + tail)], radius=sc(1.4), fill=col)
    circle(d, (cx, length + tail - 4), 3.2, fill=col)
    circle(d, (cx, length), 4.5, fill=col)
    circle(d, (cx, length), 1.8, fill=(30, 20, 18))
    return img


def cap():
    w = 26
    img = Image.new("RGBA", (sc(w), sc(w)), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    circle(d, (w / 2, w / 2), 10, fill=GOLD_DK)
    circle(d, (w / 2, w / 2), 9, fill=GOLD)
    circle(d, (w / 2, w / 2), 5, fill=COPPER)
    circle(d, (w / 2, w / 2), 2.2, fill=(20, 16, 14))
    return img


def sun():
    w = 22
    img = Image.new("RGBA", (sc(w), sc(w)), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    col = (255, 200, 90)
    for i in range(8):
        a = i * 45
        d.line([tuple(map(sc, polar(7.2, a, (w / 2, w / 2)))), tuple(map(sc, polar(10, a, (w / 2, w / 2))))],
               fill=col, width=int(sc(1.8)))
    circle(d, (w / 2, w / 2), 5.2, fill=col)
    return img


def moon():
    w = 22
    img = Image.new("RGBA", (sc(w), sc(w)), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    circle(d, (w / 2, w / 2), 8, fill=(205, 216, 236, 255))
    circle(d, (w / 2 + 4.2, w / 2 - 3.0), 7.2, fill=(0, 0, 0, 0))
    return img


HANDS = {
    # name: (builder, pivot_y_px)  pivot x is the image's centre
    "minute": (lambda: sword(20, 186, 26, 7.0, 0, GOLD, LUME), 186),
    "hour": (lambda: sword(24, 118, 22, 9.5, 0, GOLD, META_BLUE, inset=1.6), 118),
    "work": (lambda: skeleton(30, 120, 22, 7.5, 0, COPPER), 120),
    "second": (seconds_hand, 38),
}


# ---------------------------------------------------------------- encoding

def rgb565(c):
    r, g, b = c[:3]
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def c_bytes(name, data, per_line=24):
    lines = [", ".join(f"0x{b:02x}" for b in data[i:i + per_line]) for i in range(0, len(data), per_line)]
    return f"static const uint8_t {name}[{len(data)}] = {{\n    " + ",\n    ".join(lines) + "\n};\n"


def argb_bytes(img, bleed=(0, 0, 0)):
    """LVGL ARGB8888: B, G, R, A per pixel, straight alpha. Fully transparent
    pixels take the `bleed` colour, so LVGL's filtering while it rotates a
    hand doesn't pull a dark fringe in from them."""
    out = bytearray()
    for r, g, b, a in img.get_flattened_data():
        if a == 0:
            r, g, b = bleed
        out += bytes((b, g, r, a))
    return bytes(out)


def main():
    out_dir, prev_dir = Path(sys.argv[1]), Path(sys.argv[2])
    prev_dir.mkdir(parents=True, exist_ok=True)

    dial = render_dial()
    dial.save(prev_dir / "dial.png")
    q = dial.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()[: 256 * 3]
    pal += [0] * (256 * 3 - len(pal))
    idx = q.tobytes()
    assert len(idx) == W * W
    # LVGL I8: 256 lv_color32_t (B, G, R, A), then the indices. Colours go
    # through RGB565 first, so the board shows exactly what it did when the
    # dial was RGB565.
    lv_pal = bytearray()
    for i in range(256):
        v = rgb565(pal[i * 3:i * 3 + 3])
        r5, g6, b5 = v >> 11, (v >> 5) & 63, v & 31
        lv_pal += bytes(((b5 << 3) | (b5 >> 2), (g6 << 2) | (g6 >> 4), (r5 << 3) | (r5 >> 2), 255))
    dial_i8 = bytes(lv_pal) + idx
    q.convert("RGB").save(prev_dir / "dial-quantized.png")

    hands = {}
    for name, (build, pivot) in HANDS.items():
        hands[name] = (finish(build()), pivot)
    icons = {"cap": finish(cap()), "sun": finish(sun()), "moon": finish(moon())}

    src = [
        "/* Generated by firmware/tools/render_watch.py. Do not edit. */",
        '#include "watch_assets.h"', "",
        c_bytes("dial_i8", dial_i8),
        f"const watch_indexed_t watch_dial = {{ {W}, {W}, sizeof(dial_i8), dial_i8 }};", "",
    ]
    bleeds = {"minute": GOLD, "hour": GOLD, "work": COPPER, "second": (255, 104, 62), "cap": GOLD,
              "sun": (255, 200, 90), "moon": (205, 216, 236)}
    def img_dsc(name, img, pivot_x=None, pivot_y=None):
        data = argb_bytes(img, bleeds[name])
        w, h = img.size
        src.append(c_bytes(f"{name}_px", data))
        src.append(f"const watch_image_t watch_{name} = {{ {w}, {h}, {pivot_x if pivot_x is not None else w // 2}, "
                   f"{pivot_y if pivot_y is not None else h // 2}, {name}_px }};\n")
    for name, (img, pivot) in hands.items():
        img_dsc(name, img, img.size[0] // 2, pivot)
    for name, img in icons.items():
        img_dsc(name, img)
    (out_dir / "watch_assets.c").write_text("\n".join(src))

    total_hands = sum(i.size[0] * i.size[1] * 4 for i, _ in hands.values()) + sum(i.size[0] * i.size[1] * 4 for i in icons.values())
    print(f"dial: {len(idx)} px, {len(set(idx))} colours, I8 {len(dial_i8)} bytes")
    print(f"hands+icons: {total_hands} bytes ARGB8888")

    # Previews at a few times, with the board's own fonts for the live text.
    f20 = ImageFont.truetype(str(MONTSERRAT), 20)
    f16 = ImageFont.truetype(str(MONTSERRAT), 16)
    base = q.convert("RGB")
    def paste_rot(canvas, img, pivot, centre, deg):
        # rotate about the pivot: pad so the pivot is the centre, then rotate
        w, h = img.size
        px, py = w // 2, pivot
        size = 2 * max(px, w - px, py, h - py) + 2
        pad = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        pad.paste(img, (size // 2 - px, size // 2 - py))
        rot = pad.rotate(-deg, resample=Image.BICUBIC)
        canvas.alpha_composite(rot, (int(round(centre[0] - size / 2)), int(round(centre[1] - size / 2))))
    def label(canvas, xy, s, f, col):
        dr = ImageDraw.Draw(canvas)
        bb = dr.textbbox((0, 0), s, font=f)
        dr.text((xy[0] - (bb[2] - bb[0]) / 2 - bb[0], xy[1] - (bb[3] - bb[1]) / 2 - bb[1]), s, font=f, fill=col)
    for tag, (lh, lm, ls, hh, date, lday, hday) in {
        "1009": (10, 9, 36, 13, "SAT 21", True, True),
        "1950": (19, 50, 12, 22, "SUN 22", False, False),
        "0412": (4, 12, 48, 7, "MON 23", False, True),
    }.items():
        cv = base.convert("RGBA")
        label(cv, (C, DATE_Y), date, f16, IVORY)
        for x, code, day, col in ((C - WIN_X, "MIA", lday, IVORY), (C + WIN_X, "SFO", hday, COPPER)):
            icon = icons["sun" if day else "moon"]
            cv.alpha_composite(icon, (int(x - 46), int(C - 11)))
            label(cv, (x + 12, C), code, f20, col)
        paste_rot(cv, hands["second"][0], hands["second"][1], SUBDIAL, ls * 6)
        paste_rot(cv, hands["work"][0], hands["work"][1], (C, C), (hh % 12 + lm / 60) * 30)
        paste_rot(cv, hands["hour"][0], hands["hour"][1], (C, C), (lh % 12 + lm / 60) * 30)
        paste_rot(cv, hands["minute"][0], hands["minute"][1], (C, C), (lm + ls / 60) * 6)
        capi = icons["cap"]
        cv.alpha_composite(capi, (int(C - capi.size[0] / 2), int(C - capi.size[1] / 2)))
        m = Image.new("L", cv.size, 0)
        ImageDraw.Draw(m).ellipse([0, 0, W - 1, W - 1], fill=255)
        out = Image.new("RGB", cv.size, (40, 40, 40))
        out.paste(cv.convert("RGB"), (0, 0), m)
        out.save(prev_dir / f"watch-{tag}.png")
    print(f"previews in {prev_dir}")


if __name__ == "__main__":
    main()
