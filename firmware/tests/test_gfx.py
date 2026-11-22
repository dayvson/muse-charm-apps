#!/usr/bin/env python3
"""Lua apps' images and sound on the host: test_gfx.py GFX_CLI

PNGs of every colour type and bit depth (made with Pillow) must decode to
exactly Pillow's pixels at RGB565 (and alpha); broken and unsupported files
must fail cleanly; melodies must parse to the right pitches and lengths.
"""
import base64
import io
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

from PIL import Image

CLI = sys.argv[1]
TMP = Path(tempfile.mkdtemp())
failures = 0


def fail(msg):
    global failures
    failures += 1
    print("FAIL:", msg)


def run_decode(data, *extra):
    src, out = TMP / "in.bin", TMP / "out.bin"
    src.write_bytes(data)
    subprocess.run([CLI, "decode", str(src), str(out), *map(str, extra)], check=True)
    raw = out.read_bytes()
    head, _, body = raw.partition(b"\n")
    if head.startswith(b"error:"):
        return head.decode()
    w, h, a = map(int, head.split())
    return w, h, a, body


def expect565(img):
    rgba = img.convert("RGBA")
    w, h = rgba.size
    px = list(rgba.get_flattened_data() if hasattr(rgba, "get_flattened_data") else rgba.getdata())
    col = b"".join(struct.pack("<H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)) for r, g, b, _ in px)
    alpha = bytes(a for *_, a in px)
    return w, h, col, alpha


def check_png(name, img, expect=None, **save):
    buf = io.BytesIO()
    img.save(buf, "PNG", **save)
    got = run_decode(buf.getvalue())
    if isinstance(got, str):
        return fail(f"{name}: {got}")
    w, h, a, body = got
    # What the file holds, as Pillow reads it back (tRNS applied).
    ew, eh, col, alpha = expect565(expect or Image.open(io.BytesIO(buf.getvalue())))
    if (w, h) != (ew, eh):
        return fail(f"{name}: size {w}x{h}, expected {ew}x{eh}")
    has_alpha = any(x != 255 for x in alpha)
    if a != has_alpha:
        return fail(f"{name}: alpha flag {a}, expected {has_alpha}")
    if body[: w * h * 2] != col:
        return fail(f"{name}: colour mismatch")
    if a and body[w * h * 2 :] != alpha:
        return fail(f"{name}: alpha mismatch")
    print(f"ok  {name} ({w}x{h}{', alpha' if a else ''})")


def gradient(w, h):
    img = Image.new("RGBA", (w, h))
    img.putdata([((x * 255) // max(w - 1, 1), (y * 255) // max(h - 1, 1), (x * y) % 256, (x + 3 * y) % 256)
                 for y in range(h) for x in range(w)])
    return img


g = gradient(37, 23)   # odd sizes exercise partial bytes and every filter
check_png("RGBA 8", g)
check_png("RGB 8", g.convert("RGB"))
check_png("grey 8", g.convert("L"))
check_png("grey+alpha 8", g.convert("LA"))
check_png("palette 8", g.convert("RGB").quantize(200))
pal = g.convert("RGB").quantize(16)
check_png("palette 4", pal, bits=4)
check_png("palette 2", g.convert("RGB").quantize(4), bits=2)
check_png("grey 1 (bilevel)", g.convert("1"))
pt = g.convert("RGB").quantize(8)
pt.info["transparency"] = 3
check_png("palette + tRNS", pt, transparency=3)
lt = g.convert("L")
check_png("grey + tRNS key", lt, transparency=int(lt.getpixel((5, 5))))
rt = g.convert("RGB")
check_png("RGB + tRNS key", rt, transparency=rt.getpixel((0, 0)))
grey8 = g.convert("L")
g16 = Image.frombytes("I;16", grey8.size, b"".join(struct.pack("<H", v * 257) for v in grey8.tobytes()))
check_png("grey 16", g16, expect=grey8)
check_png("big RGB", gradient(300, 200).convert("RGB"), optimize=True)
check_png("full screen RGBA", gradient(466, 466))

# 16-bit RGB(A), which Pillow doesn't write: build it by hand.
def png_chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)


def rgb16_png(w, h, alpha):
    rows = b""
    for y in range(h):
        rows += b"\x00" + b"".join(struct.pack(">HHH" + ("H" if alpha else ""), *(
            [x * 3000, y * 5000, 40000] + ([y * 9000 + 100] if alpha else []))) for x in range(w))
    ihdr = struct.pack(">IIBBBBB", w, h, 16, 6 if alpha else 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr) + png_chunk(b"IDAT", zlib.compress(rows))
            + png_chunk(b"IEND", b""))


for alpha in (False, True):
    data = rgb16_png(7, 5, alpha)
    got = run_decode(data)
    exp = Image.open(io.BytesIO(data))
    exp.load()
    if isinstance(got, str):
        fail(f"RGB{'A' if alpha else ''} 16: {got}")
    else:
        w, h, a, body = got
        ok = (w, h) == (7, 5) and a == alpha
        for y in range(5):
            for x in range(7):
                r, gg, b = (x * 3000) >> 8, (y * 5000) >> 8, 40000 >> 8
                c = struct.unpack_from("<H", body, 2 * (y * 7 + x))[0]
                ok &= c == (((r & 0xF8) << 8) | ((gg & 0xFC) << 3) | (b >> 3))
                if alpha:
                    ok &= body[70 + y * 7 + x] == (y * 9000 + 100) >> 8
        print(f"{'ok ' if ok else 'FAIL'} RGB{'A' if alpha else ''} 16 (hand-built)")
        failures += not ok

# Base64 and data: URLs decode the same as the bytes.
buf = io.BytesIO()
g.save(buf, "PNG")
raw = run_decode(buf.getvalue())
for label, data in (("base64", base64.b64encode(buf.getvalue())),
                    ("base64 wrapped", base64.encodebytes(buf.getvalue())),
                    ("data URL", b"data:image/png;base64," + base64.b64encode(buf.getvalue()))):
    got = run_decode(data)
    print(f"{'ok ' if got == raw else 'FAIL'} {label}")
    failures += got != raw

# Failures, each with a clear message and no crash (the build runs ASan).
inter = bytearray(buf.getvalue())
inter[28] = 1   # IHDR's interlace byte (Pillow doesn't write interlaced PNGs)
bad_cases = {
    "interlaced": (bytes(inter), "interlaced"),
    "truncated": (buf.getvalue()[:200], "PNG"),
    "corrupt data": (buf.getvalue()[:60] + bytes(200) + buf.getvalue()[260:], "PNG"),
    "not an image": (b"hello world, not an image", "not a PNG or JPEG"),
    "too large": (None, "too large"),
    "over budget": (None, "memory is full"),
    "jpeg on host": (b"\xff\xd8\xff\xe0" + bytes(64), "JPEG"),
}
big = io.BytesIO()
Image.new("RGB", (500, 20)).save(big, "PNG")
bad_cases["too large"] = (big.getvalue(), "too large")
for name, (data, want) in bad_cases.items():
    extra = (466, 1000) if name == "over budget" else ()
    got = run_decode(data if name != "over budget" else buf.getvalue(), *extra)
    ok = isinstance(got, str) and want in got
    print(f"{'ok ' if ok else 'FAIL'} rejects {name}: {got if isinstance(got, str) else 'decoded!'}")
    failures += not ok

# Random corruption never crashes the decoder.
import random
rnd = random.Random(7)
src = buf.getvalue()
for i in range(300):
    b = bytearray(src)
    for _ in range(rnd.randint(1, 8)):
        b[rnd.randrange(8, len(b))] = rnd.randrange(256)
    run_decode(bytes(b[: rnd.randint(9, len(b))]))
print("ok  300 corrupted PNGs decoded or rejected without a crash")

# Sprite: transparent where the palette has no entry, scaled 3x.
out = TMP / "sprite.bin"
subprocess.run([CLI, "sprite", str(out)], check=True)
head, _, body = out.read_bytes().partition(b"\n")
w, h, a = map(int, head.split())
rows = ["..#..", ".###.", "#####", "#.R.#"]
ok = (w, h, a) == (15, 12, 1)
for y in range(h):
    for x in range(w):
        ch = rows[y // 3][x // 3]
        c = struct.unpack_from("<H", body, 2 * (y * w + x))[0]
        al = body[w * h * 2 + y * w + x]
        if ch == ".":
            ok &= al == 0
        else:
            rgb = 0x5AC8FA if ch == "#" else 0xFF0000
            r, gg, b = rgb >> 16, (rgb >> 8) & 255, rgb & 255
            ok &= al == 255 and c == (((r & 0xF8) << 8) | ((gg & 0xFC) << 3) | (b >> 3))
print(f"{'ok ' if ok else 'FAIL'} sprite")
failures += not ok

# Launcher icons: 96 x 96 RGB565A8 in a circle.
def make_icon(img):
    buf = io.BytesIO()
    img.save(buf, "PNG")
    src, out = TMP / "icon.png", TMP / "icon.bin"
    src.write_bytes(buf.getvalue())
    subprocess.run([CLI, "icon", str(src), str(out)], check=True)
    data = out.read_bytes()
    assert len(data) == 4 + 96 * 96 * 3 and data[:3] == b"LIC", "icon file"
    col = [struct.unpack_from("<H", data, 4 + 2 * i)[0] for i in range(96 * 96)]
    alpha = data[4 + 96 * 96 * 2:]
    return data[3], col, alpha


def px565(c):
    return ((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)


def at(x, y):
    return y * 96 + x


ok = True
# An opaque photo-like image fills the circle: opaque inside, clear outside,
# a soft edge, and cropped to the middle (a wide image loses its sides).
wide = Image.new("RGB", (300, 100), (255, 0, 0))
wide.paste((0, 0, 255), (125, 0, 175, 100))   # blue middle sixth: icon x 24..72
flags, col, alpha = make_icon(wide)
ok &= flags == 1
ok &= alpha[at(48, 48)] == 255 and alpha[at(0, 0)] == 0 and alpha[at(95, 95)] == 0
ok &= alpha[at(48, 1)] == 255 and 0 < alpha[at(48, 0)] < 255   # anti-aliased edge
ok &= all(alpha[at(x, y)] == 255 for y in range(96) for x in range(96)
          if (x + 0.5 - 48) ** 2 + (y + 0.5 - 48) ** 2 < 47 ** 2)
ok &= px565(col[at(48, 48)]) == (0, 0, 255) and px565(col[at(48, 5)]) == (0, 0, 255)   # middle kept
ok &= px565(col[at(10, 48)]) == (255, 0, 0) and px565(col[at(20, 48)]) == (255, 0, 0)
ok &= px565(col[at(26, 48)]) == (0, 0, 255) and px565(col[at(70, 48)]) == (0, 0, 255)
print(f"{'ok ' if ok else 'FAIL'} icon: opaque image fills the circle")
failures += not ok

# Pixel art with transparency: a 12 px sprite becomes 72 px (x6), crisp, centred.
ok = True
spr = Image.new("RGBA", (12, 12), (0, 0, 0, 0))
for y in range(12):
    for x in range(12):
        if (x + y) % 2 == 0:
            spr.putpixel((x, y), (255, 255, 0, 255))
flags, col, alpha = make_icon(spr)
ok &= flags == 0
for y in range(96):
    for x in range(96):
        inside = 12 <= x < 84 and 12 <= y < 84
        want = 255 if inside and ((x - 12) // 6 + (y - 12) // 6) % 2 == 0 else 0
        if (x + 0.5 - 48) ** 2 + (y + 0.5 - 48) ** 2 > 47 ** 2:
            want = 0 if not inside else alpha[at(x, y)]   # the circle trims its corners
        if alpha[at(x, y)] != want:
            ok = False
        if want and px565(col[at(x, y)]) != (255, 255, 0):
            ok = False
print(f"{'ok ' if ok else 'FAIL'} icon: 12 px sprite enlarged x6, crisp, centred")
failures += not ok

# A big transparent image shrinks to fit 72 px, averaged (soft alpha at its edges).
ok = True
big = Image.new("RGBA", (400, 200), (0, 0, 0, 0))
big.paste((0, 255, 0, 255), (0, 0, 400, 200))
flags, col, alpha = make_icon(big)
ok &= flags == 1   # fully opaque after all: fills
big.putpixel((0, 0), (0, 0, 0, 0))
flags, col, alpha = make_icon(big)
ok &= flags == 0 and alpha[at(48, 48)] == 255 and alpha[at(48, 20)] == 0   # 72 x 36, centred
ok &= px565(col[at(48, 48)]) == (0, 255, 0) and alpha[at(12, 48)] == 255 and alpha[at(11, 48)] == 0
print(f"{'ok ' if ok else 'FAIL'} icon: large transparent image fits 72 px")
failures += not ok

# Melodies.
def melody(s, bpm=120):
    return subprocess.run([CLI, "melody", s, str(bpm)], check=True, capture_output=True, text=True).stdout


got = melody("A4:4 C5:8 C#5:8. R:2 Bb3:16 C:1")
want = "440 500\n523 250\n554 375\n0 1000\n233 125\n131 2000\n"
print(f"{'ok ' if got == want else 'FAIL'} melody pitches and lengths")
failures += got != want
got = melody("C4:8 E G", 240)
want = "262 125\n330 125\n392 125\n"
print(f"{'ok ' if got == want else 'FAIL'} melody carries octave and length")
failures += got != want
for bad in ("H4", "C4:3", "C4x", "", "C4:8 Q"):
    got = melody(bad)
    ok = got.startswith("error:")
    print(f"{'ok ' if ok else 'FAIL'} melody rejects {bad!r}: {got.strip()}")
    failures += not ok
got = melody(" ".join(["C4:16"] * 129))
ok = "more than 128" in got
print(f"{'ok ' if ok else 'FAIL'} melody limit: {got.strip()}")
failures += not ok

r = subprocess.run([CLI, "synth"], capture_output=True, text=True)
print(r.stdout.strip())
failures += r.returncode != 0

print("images and sound: all OK" if not failures else f"images and sound: {failures} FAILED")
sys.exit(1 if failures else 0)
