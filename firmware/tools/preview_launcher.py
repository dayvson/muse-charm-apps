#!/usr/bin/env python3
"""Preview the Apps launcher (mirrors luapp_ui.c's rebuild()):
preview_launcher.py OUT_DIR. Writes launcher-N-pP.png (N apps, page P) and
launcher-sheet.png, and reports anything that pokes outside the round
screen. Sample icons go through the firmware's own icon maker (gfx_cli,
built here), so they look as they will on the board."""
import io
import math
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
MONT = ROOT.parent / "muse-gadget-sdk/esp32/components/muse/eink/fonts/montserrat.ttf"
SCREEN, ICON, PITCH_X, PITCH_Y, ROW0_Y, ONE_ROW_Y, NAME_W, PER_PAGE = 466, 96, 128, 124, 163, 225, 124, 6
TEAL, DIM, DOT_ON, DOT_OFF = (0x27, 0xD3, 0xA2), (0x8E, 0x8E, 0x93), (0xA7, 0x7D, 0xFF), (0x3A, 0x33, 0x58)
APPS = [("Pomodoro", 0xFF6B6B), ("Snake", 0x1C7C3A), ("Tides", 0x0A84FF), ("Breathe", 0x5AC8FA),
        ("Dice Roll", 0xFFD60A), ("Pong", 0xBF5AF2), ("Moon Phase", 0x2C2C54), ("Flashcards", 0xFF9F0A),
        ("Metronome", 0x30D158)]


def font(px):
    return ImageFont.truetype(str(MONT), px)


# ---- sample icons (original art), made into icon files by the firmware code ----

def sample_sources():
    snake = ["............", ".....GGGG...", "....G....G..", "....G.E..G..", ".....GGGGG..",
             "........G...", "...GGGGG....", "..G.........", "..G.....GGG.", "...GGGGG..R.",
             "............", "............"]
    spr = Image.new("RGBA", (12, 12), (0, 0, 0, 0))
    for y, row in enumerate(snake):
        for x, ch in enumerate(row):
            if ch != ".":
                spr.putpixel((x, y), {"G": (180, 255, 120, 255), "E": (20, 20, 20, 255), "R": (255, 80, 80, 255)}[ch])
    tides = Image.new("RGB", (96, 96), (8, 40, 90))
    d = ImageDraw.Draw(tides)
    for i, c in enumerate([(20, 110, 200), (60, 160, 240), (170, 225, 255)]):
        y = 44 + i * 14
        d.polygon([(0, 96)] + [(x, y + 7 * math.sin((x + i * 20) / 10)) for x in range(0, 97, 4)] + [(96, 96)], fill=c)
    d.ellipse((58, 14, 80, 36), fill=(255, 220, 120))
    moon = Image.new("RGB", (200, 200), (14, 14, 40))
    d = ImageDraw.Draw(moon)
    d.ellipse((40, 40, 160, 160), fill=(235, 235, 220))
    d.ellipse((72, 30, 182, 150), fill=(14, 14, 40))
    for x, y in ((20, 30), (170, 60), (150, 170), (40, 150), (100, 20)):
        d.ellipse((x, y, x + 4, y + 4), fill=(255, 255, 255))
    return {"Snake": spr, "Tides": tides, "Moon Phase": moon}


def build_cli(tmp):
    cli = tmp / "gfx_cli"
    fw = ROOT / "firmware"
    subprocess.run(["cc", "-std=c11", "-O1", f"-I{fw}", str(ROOT / "tests/gfx_cli.c"), str(fw / "luapp_image.c"),
                    str(fw / "luapp_synth.c"), "-lz", "-lm", "-o", str(cli)], check=True)
    return cli


def icon_image(cli, tmp, img):
    src, out = tmp / "icon.png", tmp / "icon.bin"
    img.save(src, "PNG")
    subprocess.run([str(cli), "icon", str(src), str(out)], check=True)
    data = out.read_bytes()
    rgba = Image.new("RGBA", (ICON, ICON))
    px = []
    for i in range(ICON * ICON):
        c = struct.unpack_from("<H", data, 4 + 2 * i)[0]
        px.append(((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31,
                   data[4 + ICON * ICON * 2 + i]))
    rgba.putdata(px)
    return rgba, bool(data[3] & 1)


# ---- the launcher ----

def disc(colr):
    """The app's colour, lit from above (bg_grad to 62%), as a circle."""
    r, g, b = colr >> 16, (colr >> 8) & 255, colr & 255
    grad = Image.new("RGBA", (ICON, ICON))
    grad.putdata([tuple(int(v * (1 - 0.38 * y / (ICON - 1))) for v in (r, g, b)) + (255,)
                  for y in range(ICON) for _ in range(ICON)])
    mask = Image.new("L", (ICON * 4, ICON * 4), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, ICON * 4 - 1, ICON * 4 - 1), fill=255)
    grad.putalpha(mask.resize((ICON, ICON), Image.LANCZOS))
    return grad


def chevron(d, cx, top, color, down=True, w=10, h=6):
    ys = (top, top + h) if down else (top + h, top)
    d.line([(cx - w, ys[0]), (cx, ys[1]), (cx + w, ys[0])], fill=color, width=3, joint="curve")


def render(n, page, icons, out):
    img = Image.new("RGB", (SCREEN, SCREEN), "black")
    d = ImageDraw.Draw(img)
    issues = []

    def inside(x, y):
        if (x - 233) ** 2 + (y - 233) ** 2 > 233 ** 2:
            issues.append((round(x), round(y)))

    pages = max(1, -(-n // PER_PAGE))
    first = page * PER_PAGE
    k = min(PER_PAGE, n - first)
    rows = 2 if k > 3 else 1
    y0 = ROW0_Y if rows == 2 else ONE_ROW_Y
    f16 = font(16)
    if page == 0:
        t = "APPS BY MUSE"
        tw = d.textlength(t, font=f16) + 2 * len(t)
        ty = y0 - ICON // 2 - 34 if n else 36
        d.text((233 - tw / 2, ty), t, font=f16, fill=TEAL)
        inside(233 - tw / 2, ty)
        inside(233 + tw / 2, ty)
    else:
        chevron(d, 233, y0 - ICON // 2 - 36 + 5, DIM, down=False, w=7, h=5)
    for j in range(k):
        row, col = divmod(j, 3)
        in_row = min(k, 3) if row == 0 else k - 3
        cx = 233 + PITCH_X * col - PITCH_X // 2 * (in_row - 1)
        cy = y0 + row * PITCH_Y
        name, colr = APPS[first + j]
        ic = icons.get(name)
        if not (ic and ic[1]):
            img.paste(disc(colr), (cx - 48, cy - 48), disc(colr))
        if ic:
            img.paste(ic[0], (cx - 48, cy - 48), ic[0])
        else:
            words = name.split()
            ini = words[0][0].upper() + (words[1][0].upper() if len(words) > 1 else name[1].lower())
            lum = ((colr >> 16) * 299 + ((colr >> 8) & 255) * 587 + (colr & 255) * 114) // 1000
            f28 = font(28)
            d.text((cx - d.textlength(ini, font=f28) / 2, cy - 17), ini, font=f28, fill="black" if lum > 150 else "white")
        rim = Image.new("RGBA", (ICON, ICON), (0, 0, 0, 0))
        ImageDraw.Draw(rim).ellipse((0, 0, ICON - 1, ICON - 1), outline=(255, 255, 255, 46), width=2)
        img.paste(rim, (cx - 48, cy - 48), rim)
        nw = min(d.textlength(name, font=f16), NAME_W)
        d.text((cx - nw / 2, cy + 52), name, font=f16, fill="white")
        for ang in range(0, 360, 10):
            inside(cx + 48 * math.cos(math.radians(ang)), cy + 48 * math.sin(math.radians(ang)))
        inside(cx - nw / 2, cy + 52 + 20)
        inside(cx + nw / 2, cy + 52 + 20)
    if page < pages - 1:
        chevron(d, 233, 376 + 6, TEAL)
        inside(233, 376 + 20)
    if pages > 1:
        for i in range(pages):
            y = 233 + 16 * i - 8 * (pages - 1)
            d.ellipse((466 - 14 - 8, y - 4, 466 - 14, y + 4), fill=DOT_ON if i == page else DOT_OFF)
            inside(466 - 14, y)
    mask = Image.new("L", (SCREEN, SCREEN), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, SCREEN - 1, SCREEN - 1), fill=255)
    bg = Image.new("RGB", (SCREEN, SCREEN), (40, 40, 40))
    bg.paste(img, (0, 0), mask)
    bg.save(out)
    return issues, bg


outdir = Path(sys.argv[1])
outdir.mkdir(parents=True, exist_ok=True)
for old in outdir.glob("launcher-*.png"):
    old.unlink()
with tempfile.TemporaryDirectory() as t:
    tmp = Path(t)
    cli = build_cli(tmp)
    icons = {name: icon_image(cli, tmp, src) for name, src in sample_sources().items()}
bad = 0
shots = []
for n, page in ((3, 0), (6, 0), (9, 0), (9, 1)):
    name = f"launcher-{n}-p{page + 1}.png"
    issues, im = render(n, page, icons, outdir / name)
    shots.append(im)
    print(f"{name}: {'fits the circle' if not issues else f'{len(issues)} points outside: {issues[:4]}'}")
    bad += bool(issues)
sheet = Image.new("RGB", (SCREEN * len(shots) + 20 * (len(shots) - 1), SCREEN), (24, 24, 24))
for i, im in enumerate(shots):
    sheet.paste(im, (i * (SCREEN + 20), 0))
sheet.save(outdir / "launcher-sheet.png")
sys.exit(bad)
