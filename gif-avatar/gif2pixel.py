#!/usr/bin/env python3
"""Convert avatar animation clips (MP4 or GIF) into a self-contained Muse `muse_pixel.c`.

    gif2pixel.py CLIP_DIR -o muse_pixel.c [options]

CLIP_DIR holds one clip per state: .mp4/.mov/.webm (via imageio-ffmpeg) or
.gif/.webp/.png. Two naming schemes are understood:

    idle.gif  working.gif  listening.gif ...         a file per clip name
    vela.mp4  vela-working.mp4  vela-darkmode-idle.mp4 ...
                                                     Muse's: STEM[-darkmode][-CLIP],
                                                     no CLIP meaning idle

Clip names and the renderer state each one plays (only idle is required):
    idle                    IDLE, and the stand-in for every missing state
    waking, boot, connecting  BOOT (first found, in that order; played once, then idle)
    listening               LISTENING   (else idle + expanding dotted rings)
    thinking                THINKING    (else working, else idle)
    working                 THINKING while pose->working (else thinking)
    speaking                SPEAKING    (else idle bouncing with the voice + a ring)
    happy, making_something the pet reaction (else a hop)
    error                   ERROR       (else idle tinted red, a shake and a "!")
    off                     OFF         (played once; else idle dimming)
Change any of this with --map CLIP=STATE (STATE none drops it), a --map-file of
CLIP=STATE lines, or point a state at a file with --clip STATE=PATH.

When a clip comes in both, the -darkmode- one is used (--prefer light to
change): the screen is black, so a dark stage keys out cleanly.

Pipeline: the flat stage colour of each clip is found on its border and keyed
to the black background (soft edge, colour decontaminated); the avatar is
cropped (union bounding box, top-square for the face, or explicit, with
--zoom), fitted into the 64x64 grid, resampled in time to --fps (default 12),
mapped onto one shared palette, and embedded as const 8-bit indices
(PackBits where smaller). If the frame data is over --budget-kb (768), the
frame rate is lowered until it fits. The renderer draws the grid as solid
blocks like the default avatar, so the UI's per-cell dirty tracking works.

Needs Pillow, and imageio-ffmpeg for video.
"""
import argparse
import collections
import datetime
import os
import re
import struct
import sys
import zlib

from PIL import Image, ImageChops, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE = os.path.join(HERE, "template.c")

W = H = 64
CELLS = W * H
STATES = ("boot", "idle", "listening", "thinking", "working", "speaking", "happy", "error", "off")
# Clip names, longest first for suffix matching, and the state each plays by default.
DEFAULT_MAP = {"milestone_level_up": "happy", "making_something": "happy", "connecting": "boot", "listening": "listening", "speaking": "speaking",
               "thinking": "thinking", "working": "working", "waking": "boot", "happy": "happy", "error": "error",
               "idle": "idle", "boot": "boot", "off": "off"}
# Several clips for one state: the first of these wins.
PRIORITY = ["idle", "boot", "waking", "connecting", "listening", "thinking", "working", "speaking", "happy",
            "milestone_level_up", "making_something", "error", "off"]
VIDEO_EXTS = (".mp4", ".mov", ".m4v", ".webm", ".mkv")
IMAGE_EXTS = (".gif", ".webp", ".png", ".apng")
MODES = ("BOOT", "IDLE", "LISTENING", "THINKING", "SPEAKING", "ERROR", "OFF")
# Copied from the default renderer (avatar/muse_pixel.c SCHEMES[].acc / WORKING_SCHEME.acc).
DEFAULT_ACCENTS = {"BOOT": 0xA9C0FF, "IDLE": 0xA77DFF, "LISTENING": 0x5CB8FF, "THINKING": 0xE07BFF,
                   "SPEAKING": 0x6FF0BF, "ERROR": 0xFF5C5C, "OFF": 0x7C72D0}
DEFAULT_WORKING_ACCENT = 0xFFB54D
# Effect colours, appended to the palette (template.c FX_*): listening accent and
# a dim one (0.45x), speaking accent, error accent (the default avatar's schemes).
FX_COLORS = [(0x5C, 0xB8, 0xFF), (0x29, 0x53, 0x73), (0x6F, 0xF0, 0xBF), (0xFF, 0x5C, 0x5C)]
RESAMPLE = {"nearest": Image.Resampling.NEAREST, "box": Image.Resampling.BOX,
            "bilinear": Image.Resampling.BILINEAR, "hamming": Image.Resampling.HAMMING,
            "bicubic": Image.Resampling.BICUBIC, "lanczos": Image.Resampling.LANCZOS}


def die(msg):
    sys.exit(f"gif2pixel: {msg}")


def log(msg=""):
    print(msg, flush=True)


# ---------------------------------------------------------------- finding clips

def parse_name(base):
    """(stem, clip, dark) from a file name without extension."""
    b = base.lower()
    dark = "darkmode" in b
    b = re.sub(r"[-_ ]?darkmode", "", b)
    for name in DEFAULT_MAP:
        if b == name:
            return "", name, dark
        if b.endswith("-" + name) or b.endswith("_" + name):
            return b[:-len(name) - 1], name, dark
    return b, "idle", dark


def discover(src, stem, prefer, prefer_format):
    """{clip name: path}, picking one stem, the preferred light/dark variant and video or GIF."""
    groups = collections.defaultdict(lambda: collections.defaultdict(dict))
    for f in sorted(os.listdir(src)):
        base, ext = os.path.splitext(f)
        if ext.lower() not in VIDEO_EXTS + IMAGE_EXTS or f.startswith("."):
            continue
        s, clip, dark = parse_name(base)
        key = "dark" if dark else "light"
        old = groups[s][clip].get(key)
        if old and (is_video(old) == (prefer_format == "video")):
            continue   # the same clip in both formats: keep the preferred one
        groups[s][clip][key] = os.path.join(src, f)
    if stem is not None:
        stem = stem.lower()
        if stem not in groups:
            die(f"no clips with stem {stem!r} in {src} (found: {', '.join(repr(s) for s in groups) or 'none'})")
    elif "" in groups:
        stem = ""          # files named by clip (idle.gif, ...) win over anything else there
    elif len(groups) == 1:
        stem = next(iter(groups))
    else:
        die(f"several avatars in {src}: {', '.join(repr(s) for s in groups)}; pick one with --stem")
    ignored = sorted(os.path.basename(p) for s, g in groups.items() if s != stem for v in g.values() for p in v.values())
    out, mixed = {}, set()
    want = "dark" if prefer == "dark" else "light"
    other = "light" if want == "dark" else "dark"
    any_want = any(want in v for v in groups[stem].values())
    for clip, variants in groups[stem].items():
        out[clip] = variants.get(want) or variants[other]
        if want not in variants and any_want:
            mixed.add(clip)   # some clips come in the preferred stage, this one doesn't
    return stem, out, ignored, mixed


def assign(clips, maps, explicit):
    """{state: (clip name, path)} from the clips found, --map and --clip."""
    mapping = dict(DEFAULT_MAP)
    mapping.update(maps)
    slots = {}
    for clip in sorted(clips, key=lambda c: PRIORITY.index(c) if c in PRIORITY else len(PRIORITY)):
        state = mapping.get(clip)
        if state in STATES and state not in slots:
            slots[state] = (clip, clips[clip])
    for state, path in explicit.items():
        slots[state] = (os.path.splitext(os.path.basename(path))[0], path)
    return slots


# ---------------------------------------------------------------- reading frames

def iter_frames(path, rng=None):
    """(RGBA image, duration ms) for every frame of a video or animated image."""
    a, b = rng or (0, None)
    if os.path.splitext(path)[1].lower() in VIDEO_EXTS:
        try:
            import imageio_ffmpeg
        except ImportError:
            die("video needs imageio-ffmpeg: "
                "toolchain/tools-venv/bin/pip install imageio-ffmpeg (scripts/00_setup.sh installs it)")
        gen = imageio_ffmpeg.read_frames(path, pix_fmt="rgb24")
        meta = next(gen)
        size = tuple(meta["size"])
        step = 1000.0 / (meta.get("fps") or 24)
        for i, buf in enumerate(gen):
            if b is not None and i >= b:
                gen.close()
                break
            if i >= a:
                yield Image.frombytes("RGB", size, bytes(buf)).convert("RGBA"), round((i + 1) * step) - round(i * step)
        return
    im = Image.open(path)
    n = getattr(im, "n_frames", 1)
    for i in range(a, min(n, b if b is not None else n)):
        im.seek(i)
        d = im.info.get("duration") or 100
        if d < 20:          # what browsers do with 0/10 ms GIF delays
            d = 100
        yield im.convert("RGBA"), int(round(d))


def is_video(path):
    return os.path.splitext(path)[1].lower() in VIDEO_EXTS


# ---------------------------------------------------------------- background

def max_channel_diff(rgb, color):
    r, g, b = ImageChops.difference(rgb, Image.new("RGB", rgb.size, color)).split()
    return ImageChops.lighter(ImageChops.lighter(r, g), b)


def border_pixels(im):
    w, h = im.size
    px = im.load()
    pts = [(x, 0) for x in range(w)] + [(x, h - 1) for x in range(w)]
    pts += [(0, y) for y in range(1, h - 1)] + [(w - 1, y) for y in range(1, h - 1)]
    return [px[p] for p in pts]


def detect_bg(frame, tol):
    """The flat stage colour on a frame's border (averaged over its near pixels), or None."""
    border = border_pixels(frame)
    opaque = [p[:3] for p in border if p[3] >= 128]
    if len(opaque) < 0.5 * len(border):
        return None   # mostly transparent border: the clip's own transparency is the background
    # Video noise: bucket colours coarsely, then average the winning bucket.
    common = collections.Counter((p[0] >> 3, p[1] >> 3, p[2] >> 3) for p in opaque).most_common(1)[0][0]
    centre = tuple(c * 8 + 4 for c in common)
    near = [p for p in opaque if max(abs(p[i] - centre[i]) for i in range(3)) <= tol]
    if len(near) < 0.6 * len(border):
        return None
    return tuple(round(sum(p[i] for p in near) / len(near)) for i in range(3))


def hard_key(frame, bg, tol, mode):
    """`frame` with its stage made transparent (hard edge): for bounding boxes and pixel art."""
    if bg is None:
        return frame
    close = max_channel_diff(frame.convert("RGB"), bg).point(lambda v: 255 if v <= tol else 0)
    if mode == "flood":
        w, h = close.size
        px = close.load()
        for x, y in [(x, 0) for x in range(w)] + [(x, h - 1) for x in range(w)] + \
                    [(0, y) for y in range(h)] + [(w - 1, y) for y in range(h)]:
            if px[x, y] == 255:
                ImageDraw.floodfill(close, (x, y), 128)
        close = close.point(lambda v: 255 if v == 128 else 0)
    out = frame.copy()
    out.putalpha(ImageChops.multiply(frame.getchannel("A"), ImageChops.invert(close)))
    return out


def soft_key(im, bg, tol, soft, mode, cut):
    """Composite a small RGBA image onto black, keying stage colour `bg` with a soft edge.

    Pixels within `tol` of the stage are background; from tol to tol+soft they fade in,
    and the stage's share of their colour is taken out (so a light stage leaves no halo
    on black). flood keys only stage connected to the border (or to transparency).
    Returns (RGB on black, background mask with 1 = background)."""
    w, h = im.size
    n = w * h
    rgb = im.convert("RGB")
    src = bytearray(rgb.tobytes())
    aa = im.getchannel("A").tobytes()
    ka = bytearray(b"\xff") * n
    if bg is not None:
        dd = max_channel_diff(rgb, bg).tobytes()
        lim = tol + soft
        passable = bytearray(1 if (aa[i] <= cut or dd[i] <= lim) else 0 for i in range(n))
        if mode == "flood":
            region = bytearray(n)
            stack = [i for i in range(n) if passable[i] and (i < w or i >= n - w or i % w in (0, w - 1))]
            for i in stack:
                region[i] = 1
            while stack:
                i = stack.pop()
                x = i % w
                for j in (i - w if i >= w else -1, i + w if i < n - w else -1,
                          i - 1 if x > 0 else -1, i + 1 if x < w - 1 else -1):
                    if j >= 0 and passable[j] and not region[j]:
                        region[j] = 1
                        stack.append(j)
        else:
            region = passable
        orig = bytes(src)
        for i in range(n):
            if region[i]:
                v = dd[i]
                k = 0 if v <= tol else (255 if v >= lim else (v - tol) * 255 // max(1, soft))
                ka[i] = k
                if 0 < k < 255:
                    for c in range(3):
                        o = src[3 * i + c] * 255 - (255 - k) * bg[c]
                        src[3 * i + c] = max(0, min(255, o // k))
        if soft:
            # The edge band: cells next to the keyed stage are part stage (the downscale mixed
            # them), often too far from the stage colour for the ramp. Estimate their coverage
            # from the avatar colour just inside, c = a * inner + (1 - a) * stage, and take the
            # stage's share out.
            def near(i, r):
                x, y = i % w, i // w
                for yy in range(max(0, y - r), min(h, y + r + 1)):
                    for xx in range(max(0, x - r), min(w, x + r + 1)):
                        yield yy * w + xx
            # (stage-side cells inside the soft ramp, and avatar-side cells touching the stage)
            band = [i for i in range(n) if aa[i] > cut and
                    ((region[i] and ka[i] > 0) or (not region[i] and any(region[j] for j in near(i, 1))))]
            inband = bytearray(n)
            for i in band:
                inband[i] = 1
            fixed = {}
            for i in band:
                inner = [j for j in near(i, 2) if not region[j] and not inband[j] and aa[j] > cut]
                if not inner:
                    continue
                fg = [sum(orig[3 * j + c] for j in inner) / len(inner) for c in range(3)]
                fb = [fg[c] - bg[c] for c in range(3)]
                cb = [orig[3 * i + c] - bg[c] for c in range(3)]
                den = sum(v * v for v in fb)
                if den < 64:
                    continue
                a = max(0.0, min(1.0, sum(cb[c] * fb[c] for c in range(3)) / den))
                fixed[i] = (int(a * 255), [max(0, min(255, round((orig[3 * i + c] - (1 - a) * bg[c]) / max(a, 1e-3))))
                                           for c in range(3)])
            for i, (k, col) in fixed.items():
                ka[i] = k
                src[3 * i:3 * i + 3] = bytes(col)
    out = bytearray(3 * n)
    mask = bytearray(n)
    for i in range(n):
        a = aa[i] * ka[i] // 255
        if a <= cut:
            mask[i] = 1
            continue
        for c in range(3):
            out[3 * i + c] = src[3 * i + c] * a // 255
    return Image.frombytes("RGB", (w, h), bytes(out)), Image.frombytes("L", (w, h), bytes(mask))


# ---------------------------------------------------------------- pixel-art grid detection

def detect_cell(frames, max_k=32):
    """Integer upscale factor and phase (k, ox, oy) of nearest-neighbour upscaled pixel art, or None.

    Colour changes between neighbouring source pixels of upscaled art happen only at
    cell boundaries (one phase mod k), or at two adjacent phases when the cells were
    drawn with a dimmed edge (Muse's faint pixel grid)."""
    hx, hy = collections.Counter(), collections.Counter()
    for f in frames:
        rgb = f.convert("RGB")
        w, h = rgb.size
        r, g, b = ImageChops.difference(rgb.crop((0, 0, w - 1, h)), rgb.crop((1, 0, w, h))).split()
        cx = ImageChops.lighter(ImageChops.lighter(r, g), b).point(lambda v: 255 if v > 12 else 0)
        r, g, b = ImageChops.difference(rgb.crop((0, 0, w, h - 1)), rgb.crop((0, 1, w, h))).split()
        cy = ImageChops.lighter(ImageChops.lighter(r, g), b).point(lambda v: 255 if v > 12 else 0)
        for x in range(w - 1):
            hx[x] += cx.crop((x, 0, x + 1, h)).histogram()[255]
        for y in range(h - 1):
            hy[y] += cy.crop((0, y, w, y + 1)).histogram()[255]
    if sum(hx.values()) + sum(hy.values()) < 50:
        return None

    def phases(k):
        px, py = collections.Counter(), collections.Counter()
        for x, n in hx.items():
            px[x % k] += n
        for y, n in hy.items():
            py[y % k] += n
        return px, py

    def score(k, m):
        s = 0
        for c in phases(k):
            top = sorted(c.values(), reverse=True)[:m]
            s += (sum(top) / max(1, sum(c.values())) - m / k) / (1 - m / k)
        return s / 2

    for m in (1, 2):
        good = [k for k in range(m + 1, max_k + 1) if score(k, m) > 0.9]
        if good:
            k = max(good)
            px, py = phases(k)

            def start(c):
                # A cell starts right after the last change phase.
                top = [p for p, _ in c.most_common(m)]
                last = top[0]
                if m == 2 and (top[1] - top[0]) % k == 1:
                    last = top[1]
                return (last + 1) % k
            return k, start(px), start(py)
    return None


def reduce_cells(frame, k, ox, oy):
    """Sample the centre source pixel of every k x k cell."""
    w, h = frame.size
    nx, ny = (w - ox) // k, (h - oy) // k
    return frame.resize((nx, ny), Image.Resampling.NEAREST, box=(ox, oy, ox + nx * k, oy + ny * k))


# ---------------------------------------------------------------- palette and encoding

LUT5 = [((v >> 3) << 3) | (v >> 5) for v in range(256)]
LUT6 = [((v >> 2) << 2) | (v >> 6) for v in range(256)]


def to565(im):
    r, g, b = im.split()
    return Image.merge("RGB", (r.point(LUT5), g.point(LUT6), b.point(LUT5)))


def build_palette(images, masks, ncolors):
    """Up to `ncolors` colours for every avatar (non-background) pixel of all frames."""
    counts = collections.Counter()
    for im, m in zip(images, masks):
        data, mk = im.tobytes(), m.tobytes()
        for i in range(CELLS):
            if not mk[i]:
                counts[data[3 * i:3 * i + 3]] += 1
    distinct = len(counts)
    if distinct == 0:
        die("no avatar pixels left after background keying (try --bg-color none or a lower --bg-tol)")
    if distinct <= ncolors:
        return [tuple(c) for c, _ in counts.most_common()], distinct, True
    pix = bytearray()
    for c, n in counts.items():
        pix += c * n
    npx = len(pix) // 3
    width = 1024
    rows = (npx + width - 1) // width
    pix += pix[:3] * (rows * width - npx)   # pad with an existing colour
    q = Image.frombytes("RGB", (width, rows), bytes(pix)).quantize(colors=ncolors, method=Image.Quantize.MEDIANCUT)
    flat = q.getpalette()
    pal = [tuple(flat[3 * i:3 * i + 3]) for i in sorted(set(q.tobytes()))]
    pal = list(dict.fromkeys((LUT5[c[0]], LUT6[c[1]], LUT5[c[2]]) for c in pal))
    return pal, distinct, False


def palette_image(pal):
    flat = [v for c in pal for v in c] + list(pal[0]) * (256 - len(pal))
    p = Image.new("P", (1, 1))
    p.putpalette(flat)
    return p


def index_frame(im, mask, pimg, n, dither):
    q = im.quantize(palette=pimg, dither=Image.Dither.FLOYDSTEINBERG if dither else Image.Dither.NONE)
    idx = bytearray(q.tobytes().translate(bytes([(i + 1) if i < n else 1 for i in range(256)])))
    mk = mask.tobytes()
    for i in range(CELLS):
        if mk[i]:
            idx[i] = 0
    return bytes(idx)


def packbits(data):
    """c < 0x80: c+1 literal bytes follow; c >= 0x80: the next byte repeats (c & 0x7f) + 2 times."""
    out, lit = bytearray(), bytearray()
    i, n = 0, len(data)

    def flush():
        while lit:
            chunk = lit[:128]
            out.append(len(chunk) - 1)
            out.extend(chunk)
            del lit[:128]
    while i < n:
        r = 1
        while i + r < n and r < 129 and data[i + r] == data[i]:
            r += 1
        if r >= 3:
            flush()
            out.append(0x80 | (r - 2))
            out.append(data[i])
        else:
            lit.extend(data[i:i + r])
        i += r
    flush()
    return bytes(out)


def unpackbits(src, size=CELLS):
    out = bytearray()
    i = 0
    while i < len(src) and len(out) < size:
        c = src[i]
        i += 1
        if c & 0x80:
            out += bytes([src[i]]) * ((c & 0x7F) + 2)
            i += 1
        else:
            out += src[i:i + c + 1]
            i += c + 1
    return bytes(out)


def merge_dups(frames, durs):
    of, od = [], []
    for f, d in zip(frames, durs):
        if of and of[-1] == f:
            od[-1] += d
        else:
            of.append(f)
            od.append(d)
    return of, od


def decimate(frames, durs, cap):
    """At most `cap` frames over the same total time: sampled evenly in time, then repeats merged."""
    frames, durs = merge_dups(frames, durs)
    if len(frames) <= cap:
        return frames, durs
    total = sum(durs)
    ends, acc = [], 0
    for d in durs:
        acc += d
        ends.append(acc)
    out_f, out_d = [], []
    j = 0
    for s in range(cap):
        t = (s + 0.5) * total / cap
        while j < len(ends) - 1 and ends[j] <= t:
            j += 1
        out_f.append(frames[j])
        out_d.append(round((s + 1) * total / cap) - round(s * total / cap))
    return merge_dups(out_f, out_d)


# ---------------------------------------------------------------- C output

def c_float(v):
    s = f"{v:.2f}".rstrip("0")
    return s + "0" if s.endswith(".") else s


def emit_c(path, fields, palette, data, frames, clips, external=None):
    pal_lines = ["    " + " ".join("{ %d, %d, %d }," % c for c in palette[i:i + 6]) for i in range(0, len(palette), 6)]
    clip_lines, cliplist = [], []
    for s in STATES:
        c = clips.get(s)
        if c:
            first, count, total_ms, once, name = c
            clip_lines.append(f"    [CLIP_{s.upper()}] = {{ {first}, {count}, {total_ms}, "
                              f"{'true' if once else 'false'} }},   /* {name} */")
            cliplist.append(f"{s}{'' if name == s else ' (' + name + ')'} {count}f/{total_ms}ms")
        else:
            clip_lines.append(f"    [CLIP_{s.upper()}] = {{ 0, 0, 0, false }},   /* none */")
    cl, line = [], ""
    for item in cliplist:
        if line and len(line) + len(item) > 64:
            cl.append(line.rstrip(", "))
            line = ""
        line += item + ", "
    cl.append(line.rstrip(", "))
    total = len(data) + len(palette) * 3 + len(frames) * 8 + len(STATES) * 12
    crc = zlib.crc32(data) & 0xffffffff
    if external:
        # The frames go to the "avatar" partition: header (magic "MPX1", length,
        # CRC-32, 0), then the data. The C checks the header matches.
        with open(external, "wb") as f:
            f.write(struct.pack("<4I", 0x3158504D, len(data), crc, 0) + bytes(data))
        total -= len(data)
    rep = dict(fields)
    rep.update({
        "__DATE__": datetime.date.today().isoformat(),
        "__CLIPLIST__": "\n *        ".join(cl),
        "__NCOLORS__": str(len(palette) - 1 - len(FX_COLORS)),
        "__NFX__": str(len(FX_COLORS)),
        "__DATABYTES__": str(len(data)),
        "__TOTALBYTES__": str(total),
        "__PALN__": str(len(palette)),
        "__PALETTE__": "\n".join(pal_lines),
        "__DATALEN__": str(len(data)),
        "__EXTERNAL__": "1" if external else "0",
        "__DATACRC__": f"0x{crc:08x}",
        "__DATA__": "" if external else "\n".join("    " + ",".join(f"0x{b:02x}" for b in data[i:i + 24]) + ","
                              for i in range(0, len(data), 24)),
        "__NFRAMES__": str(len(frames)),
        "__FRAMES__": "\n".join(f"    {{ {o}, {ln}, {ms} }}," for o, ln, ms in frames),
        "__CLIPS__": "\n".join(clip_lines),
    })
    with open(TEMPLATE) as f:
        text = f.read()
    for k, v in rep.items():
        text = text.replace(k, v)
    left = re.findall(r"__[A-Z]+__", text)
    if left:
        die(f"template placeholders left unfilled: {sorted(set(left))}")
    with open(path, "w") as f:
        f.write(text)
    return total


# ---------------------------------------------------------------- main

def parse_color(s):
    s = s.lstrip("#")
    if len(s) != 6:
        raise argparse.ArgumentTypeError("colour must be RRGGBB")
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("src", help="directory of clips")
    ap.add_argument("-o", "--out", required=True, help="muse_pixel.c to write")
    ap.add_argument("--name", default="Muse avatar", help="avatar name for the header comment")
    g = ap.add_argument_group("clips")
    g.add_argument("--stem", help="which avatar's clips (STEM.mp4, STEM-working.mp4, ...) when there are several")
    g.add_argument("--prefer", choices=["dark", "light"], default="dark",
                   help="-darkmode- or light clips when both exist (default dark)")
    g.add_argument("--prefer-format", choices=["video", "gif"], default="video",
                   help="when a clip is there as both video and GIF/WebP, use the video (default) or the GIF")
    g.add_argument("--map", action="append", default=[], metavar="CLIP=STATE",
                   help=f"play clip CLIP as STATE ({', '.join(STATES)}, or none)")
    g.add_argument("--map-file", help="file of CLIP=STATE lines (# comments)")
    g.add_argument("--clip", action="append", default=[], metavar="STATE=PATH", help="use this file for a state")
    g.add_argument("--range", action="append", default=[], metavar="STATE=A:B",
                   help="use only source frames A..B-1 of a state's clip")
    g = ap.add_argument_group("framing")
    g.add_argument("--crop", default="auto",
                   help="auto (union bbox of the avatar in every clip), square (that bbox squared, ignoring thin "
                        "edges, padded), top-square (a square the avatar's width from its top: head and shoulders), "
                        "full, or X,Y,W,H in source pixels")
    g.add_argument("--zoom", type=float, default=1.0, help="zoom into the crop (2: half as wide), about --anchor")
    g.add_argument("--crop-pad", type=float, default=0.04, help="square crops: padding as a share of the side")
    g.add_argument("--crop-trim", type=float, default=0.1,
                   help="square crops: ignore edge rows/columns with less than this share of the busiest one's "
                        "avatar pixels (thin strings, stray sparkles)")
    g.add_argument("--auto-crop-per-clip", action="store_true",
                   help="crop every clip to its own square (the 'square' crop of its own pixels) unless set otherwise")
    g.add_argument("--crop-for", action="append", default=[], metavar="STATE=SPEC", help="crop for one clip")
    g.add_argument("--zoom-for", action="append", default=[], metavar="STATE=Z", help="zoom for one clip")
    g.add_argument("--clip-config", help="JSON {STATE or CLIP: {crop, zoom, anchor, fps, max_frames, range}}")
    g.add_argument("--anchor", choices=["top", "center", "bottom"],
                   help="where --zoom holds still (default: top for top-square, else center)")
    g.add_argument("--margin", type=int, default=2, help="empty cells around the fitted avatar (default 2)")
    g.add_argument("--resample", default="auto", choices=["auto", "pixel"] + list(RESAMPLE),
                   help="pixel: sample the source's pixel grid, integer-scaled; lanczos/box/...: smooth art; "
                        "auto: pixel if a grid is found (never for video), else lanczos")
    g.add_argument("--cell", type=int, help="source pixels per art pixel for --resample pixel (default: detect)")
    g.add_argument("--face-y", type=float,
                   help="face height within the fitted avatar, 0..1, for the effects (default 0.35, top-square 0.45)")
    g = ap.add_argument_group("background")
    g.add_argument("--bg-color", default="auto", help="auto (the flat stage on each clip's border), none, or RRGGBB")
    g.add_argument("--bg-tol", type=int, default=20, help="per-channel distance from the stage that is stage")
    g.add_argument("--bg-soft", type=int, default=28, help="width of the soft edge beyond --bg-tol (0: hard)")
    g.add_argument("--bg-key", choices=["flood", "global"], default="flood",
                   help="key only stage connected to the border (default), or every stage-coloured pixel")
    g.add_argument("--alpha-cut", type=int, default=24, help="pixels at or below this alpha become background")
    g = ap.add_argument_group("colour")
    g.add_argument("--colors", type=int, default=255 - len(FX_COLORS),
                   help=f"palette size, at most {255 - len(FX_COLORS)} (+ background + effects)")
    g.add_argument("--dither", action="store_true",
                   help="Floyd-Steinberg when mapping to the palette (may shimmer between frames)")
    g = ap.add_argument_group("frames and memory")
    g.add_argument("--fps", type=float, default=12, help="resample clips to at most this frame rate (0: keep all)")
    g.add_argument("--fps-for", action="append", default=[], metavar="STATE=FPS", help="frame rate for one clip")
    g.add_argument("--max-frames", type=int, help="cap frames per clip (decimated evenly in time)")
    g.add_argument("--budget-kb", type=float, default=768, help="max frame data in KiB (default 768)")
    g.add_argument("--no-fit", action="store_true", help="don't lower the frame rate to fit the budget (just warn)")
    g.add_argument("--external", metavar="BIN",
                   help="frame data to BIN (for the board's \"avatar\" flash partition) instead of into the C: "
                        "no PSRAM cost, so the budget can be the partition's size")
    g.add_argument("--encoding", choices=["auto", "raw", "rle"], default="auto",
                   help="auto: PackBits per frame where it's smaller")
    g.add_argument("--once", default="boot,off", help="states whose clip plays once (default boot,off)")
    g = ap.add_argument_group("behaviour")
    g.add_argument("--speak", choices=["auto", "bounce", "frames", "clip"], default="auto",
                   help="with a speaking clip: auto/clip play it as is, bounce adds a bounce with the voice, "
                        "frames picks its frames by voice level (frames ordered closed..open). Without one, "
                        "speaking is always idle bouncing with the voice")
    g.add_argument("--speak-bounce", type=float, default=2, help="voice bounce height in cells")
    g.add_argument("--speak-pulse", type=float, default=0,
                   help="idle standing in for speaking brightens by up to this share with the voice (e.g. 0.15)")
    g.add_argument("--hop", type=float, default=3, help="happy hop height in cells (no happy clip)")
    g.add_argument("--happy-hold", choices=["clip", "pet"], default="clip",
                   help="a pet plays the happy clip once through (clip, default; a mode change stops it) or "
                        "only while the pet reaction lasts (pet, ~1.6 s)")
    g.add_argument("--no-effects", action="store_true", help="no rings/tint/!/fades on idle standing in")
    g.add_argument("--accent", action="append", default=[], metavar="MODE=RRGGBB",
                   help="override a mode's accent (e.g. idle=a77dff, working=ffb54d)")
    g.add_argument("--dump", help="also write the converted 64x64 clips as 5x GIFs to this directory")
    return ap.parse_args()


def main():
    args = parse_args()
    if not 1 <= args.colors <= 255 - len(FX_COLORS):
        die(f"--colors must be 1..{255 - len(FX_COLORS)}")
    if args.zoom <= 0:
        die("--zoom must be > 0")

    # ---- which clip plays which state
    maps = {}
    lines = []
    if args.map_file:
        with open(args.map_file) as f:
            lines = [ln.split("#")[0].strip() for ln in f]
    for m in [ln for ln in lines if ln] + args.map:
        k, sep, v = (x.strip().lower() for x in m.partition("="))
        if not sep or (v not in STATES and v != "none"):
            die(f"bad mapping {m!r}: want CLIP=STATE with STATE one of {', '.join(STATES)} or none")
        maps[k] = v
    explicit = {}
    for c in args.clip:
        k, sep, v = c.partition("=")
        if not sep or k not in STATES or not os.path.isfile(v):
            die(f"bad --clip {c!r}: want STATE=PATH to an existing file")
        explicit[k] = v
    stem, found, ignored, mixed = discover(args.src, args.stem, args.prefer, args.prefer_format)
    slots = assign(found, maps, explicit)
    if "idle" not in slots:
        die(f"no idle clip in {args.src} (it's every missing state's stand-in)")
    per = per_clip_options(args, slots)
    ranges = {st: o["range"] for st, o in per.items() if "range" in o}
    once = {s.strip() for s in args.once.split(",") if s.strip()}

    log(f"Clips in {args.src}" + (f" (stem {stem!r})" if stem else ""))
    for s in STATES:
        if s in slots:
            clip, p = slots[s]
            log(f"  {s:10s} <- {os.path.basename(p)}" + (f"  [{clip}]" if clip != s else ""))
    unused = sorted(c for c in found if all(c != v[0] for v in slots.values()))
    if unused:
        log(f"  unused clips: {', '.join(unused)}")
    if mixed:
        log(f"  note: no {args.prefer} version of {', '.join(sorted(mixed))}; the other one is used")
    if ignored:
        log(f"  ignored (other names): {', '.join(ignored[:8])}{' ...' if len(ignored) > 8 else ''}")
    stand = [s for s in ("boot", "listening", "speaking", "error", "off") if s not in slots]
    if stand:
        log(f"  idle stands in for: {', '.join(stand)}" + ("" if args.no_effects else " (with effects)"))

    # ---- pass 1: sizes, stage colour, bounding-box proxies, pixel-grid samples
    log("Reading")
    info = {}
    PROXY = 128
    for s, (clip, path) in slots.items():
        n, total, size, bg, proxies, samples = 0, 0, None, None, [], []
        for im, d in iter_frames(path, ranges.get(s)):
            if size is None:
                size = im.size
                first = im
                if args.bg_color == "none":
                    bg = None
                elif args.bg_color == "auto":
                    bg = detect_bg(im, args.bg_tol)
                else:
                    bg = parse_color(args.bg_color)
            elif im.size != size:
                die(f"{path}: frame size changes mid-clip")
            pr = im.copy()
            pr.thumbnail((PROXY, PROXY), Image.Resampling.BOX)
            proxies.append(pr)
            if not is_video(path) and n < 64:
                samples.append(im)
            n += 1
            total += d
        if n == 0:
            die(f"{path}: no frames")
        info[s] = dict(path=path, n=n, ms=total, size=size, bg=bg, proxies=proxies, first=first,
                       samples=[samples[0], samples[len(samples) // 2]] if samples else [])
        log(f"  {s:10s} {size[0]}x{size[1]}, {n} frames, {total} ms ({1000 * n / max(1, total):.1f} fps), stage "
            + ("transparent" if bg is None else "#%02x%02x%02x" % bg))
    # A clip whose scene covers much of its border (a desk, a laptop) takes the stage the
    # other clips agree on, if enough of its border is that colour.
    if args.bg_color == "auto":
        found_bg = collections.Counter(v["bg"] for v in info.values() if v["bg"] is not None)
        if found_bg:
            stage = found_bg.most_common(1)[0][0]
            for s, v in info.items():
                if v["bg"] is None and v.get("first") is not None:
                    border = [p[:3] for p in border_pixels(v["first"]) if p[3] >= 128]
                    near = sum(1 for p in border if max(abs(p[i] - stage[i]) for i in range(3)) <= args.bg_tol)
                    if border and near >= 0.25 * len(border):
                        v["bg"] = stage
                        hexc = "#%02x%02x%02x" % stage
                        log(f"  {s:10s} stage taken from the other clips: {hexc} "
                            f"({100 * near // len(border)}% of its border)")
    for v in info.values():
        v.pop("first", None)
    sizes = {v["size"] for v in info.values()}
    if len(sizes) > 1:
        die(f"clips have different frame sizes {sorted(sizes)}; export them on one canvas")
    src_w, src_h = sizes.pop()

    # ---- pixel art or smooth
    mode = args.resample
    cell = None
    if mode in ("auto", "pixel"):
        samples = [f for v in info.values() for f in v["samples"]]
        if args.cell:
            cell = (args.cell, 0, 0)
        elif samples:
            cell = detect_cell(samples[:12])
        if cell and cell[0] >= 2:
            mode = "pixel"
            log(f"Pixel art: {cell[0]} source px per art pixel, grid origin ({cell[1]},{cell[2]})")
        elif mode == "pixel":
            cell = (1, 0, 0)
            log("Pixel art: no integer upscale found; each source pixel is an art pixel")
        else:
            mode = "lanczos"
            log("Smooth art: Lanczos resampling")
    else:
        log(f"Resampling: {mode}")

    avail = W - 2 * args.margin
    if avail < 8:
        die("--margin too large")
    conv = {}
    place = {}   # state -> (out_w, out_h, px0, py0) in the grid
    if mode == "pixel":
        # ---- pass 2 (pixel art): sample the grid, key the stage, crop, integer-scale
        k, ox, oy = cell
        art = {}
        for s, v in info.items():
            fr, du = [], []
            for im, d in iter_frames(v["path"], ranges.get(s)):
                fr.append(hard_key(reduce_cells(im, k, ox, oy), v["bg"], args.bg_tol, args.bg_key))
                du.append(d)
            art[s] = (fr, du)
        aw, ah = next(iter(art.values()))[0][0].size
        bbox = None
        for fr, _ in art.values():
            for f in fr:
                b = f.getchannel("A").point(lambda x: 255 if x > args.alpha_cut else 0).getbbox()
                if b:
                    bbox = b if bbox is None else (min(bbox[0], b[0]), min(bbox[1], b[1]),
                                                   max(bbox[2], b[2]), max(bbox[3], b[3]))
        if bbox is None:
            die("every frame is empty after keying the stage (try --bg-color none)")
        box = choose_box(args.crop, args.zoom, args.anchor, bbox, (aw, ah),
                         lambda x, y: ((x - ox) / k, (y - oy) / k), bbox, args.crop_pad)
        box = tuple(int(round(c)) for c in box)
        bw, bh = box[2] - box[0], box[3] - box[1]
        if bw <= W and bh <= H:
            # Art that fits the grid is never resampled: the margin gives way first.
            scale = max(1, min(avail // bw, avail // bh))
            out_w, out_h = bw * scale, bh * scale
            how = f"x{scale} nearest" + (" (into the margin)" if bw > avail or bh > avail else "")
        else:
            sc = min(avail / bw, avail / bh)
            out_w, out_h = max(1, round(bw * sc)), max(1, round(bh * sc))
            how = f"x{sc:.3f} nearest (bigger than the grid: some art pixels are dropped)"
        px0, py0 = (W - out_w) // 2, (H - out_h) // 2
        place = {s: (out_w, out_h, px0, py0) for s in info}
        log(f"Crop: {bw}x{bh} art px at ({box[0]},{box[1]}) -> {out_w}x{out_h} cells at ({px0},{py0}), {how}")
        for s, (fr, du) in art.items():
            rgbs, masks = [], []
            for f in fr:
                c = f.crop(box)
                if c.size != (out_w, out_h):
                    c = c.resize((out_w, out_h), Image.Resampling.NEAREST)
                canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0))
                canvas.paste(c, (px0, py0))
                rgb, mask = soft_key(canvas, None, 0, 0, "global", args.alpha_cut)
                rgbs.append(to565(rgb))
                masks.append(mask)
            conv[s] = (rgbs, masks, du)
    else:
        # ---- smooth: bounding boxes from the keyed proxies, then pass 2 at full resolution
        union, cols_all, rows_all = None, None, None
        for s, v in info.items():
            bbox, cols, rows = None, None, None
            for pr in v["proxies"]:
                a = hard_key(pr, v["bg"], args.bg_tol + args.bg_soft // 2, args.bg_key).getchannel("A") \
                    .point(lambda x: 255 if x > args.alpha_cut else 0)
                pw, ph = a.size
                if cols is None:
                    cols, rows = [0] * pw, [0] * ph
                for x in range(pw):
                    cols[x] += a.crop((x, 0, x + 1, ph)).histogram()[255]
                for y in range(ph):
                    rows[y] += a.crop((0, y, pw, y + 1)).histogram()[255]
                b = a.getbbox()
                if b:
                    bbox = b if bbox is None else (min(bbox[0], b[0]), min(bbox[1], b[1]),
                                                   max(bbox[2], b[2]), max(bbox[3], b[3]))
            pr_w, pr_h = v["proxies"][0].size
            v["proxies"] = None
            fx, fy = src_w / pr_w, src_h / pr_h
            v["to_src"] = (fx, fy)
            v["bbox"] = None if bbox is None else (max(0, (bbox[0] - 1) * fx), max(0, (bbox[1] - 1) * fy),
                                                   min(src_w, (bbox[2] + 1) * fx), min(src_h, (bbox[3] + 1) * fy))
            v["density"] = (cols, rows)
            if v["bbox"]:
                b = v["bbox"]
                union = b if union is None else (min(union[0], b[0]), min(union[1], b[1]),
                                                 max(union[2], b[2]), max(union[3], b[3]))
                cols_all = cols if cols_all is None else [x + y for x, y in zip(cols_all, cols)]
                rows_all = rows if rows_all is None else [x + y for x, y in zip(rows_all, rows)]
        if union is None:
            die("every frame is empty after keying the stage (try --bg-color none or a lower --bg-tol)")
        rs = RESAMPLE[mode]
        log(f"Crops ({mode}):")
        for s, v in info.items():
            o = per.get(s, {})
            own = "crop" in o
            spec = o.get("crop", args.crop)
            bbox = (v["bbox"] or union) if own else union
            density = v["density"] if own else (cols_all, rows_all)
            zoom = o.get("zoom", args.zoom)
            box = choose_box(spec, zoom, o.get("anchor", args.anchor), bbox, (src_w, src_h),
                             lambda x, y: (x, y), trimmed(density, v["to_src"], args.crop_trim) or bbox,
                             args.crop_pad)
            bw, bh = box[2] - box[0], box[3] - box[1]
            sc = min(avail / bw, avail / bh)
            out_w, out_h = max(1, round(bw * sc)), max(1, round(bh * sc))
            px0, py0 = (W - out_w) // 2, (H - out_h) // 2
            place[s] = (out_w, out_h, px0, py0)
            log(f"  {s:10s} {spec}{'' if own else ' (shared)'}" + (f" zoom {zoom:g}" if zoom != 1 else "")
                + f": {bw:.0f}x{bh:.0f} px at ({box[0]:.0f},{box[1]:.0f}) -> {out_w}x{out_h} cells at ({px0},{py0})")
            # Crop to whole pixels first (beyond the frame pads with transparency), then resample the exact box.
            ib = (int(box[0] // 1), int(box[1] // 1), int(-(-box[2] // 1)), int(-(-box[3] // 1)))
            sub = (box[0] - ib[0], box[1] - ib[1], box[2] - ib[0], box[3] - ib[1])
            rgbs, masks, du = [], [], []
            for im, d in iter_frames(v["path"], ranges.get(s)):
                c = im.crop(ib).resize((out_w, out_h), rs, box=sub, reducing_gap=3.0)
                canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0))
                canvas.paste(c, (px0, py0))
                rgb, mask = soft_key(canvas, v["bg"], args.bg_tol, args.bg_soft, args.bg_key, args.alpha_cut)
                rgbs.append(to565(rgb))
                masks.append(mask)
                du.append(d)
            conv[s] = (rgbs, masks, du)

    # ---- palette
    pal, distinct, exact = build_palette([im for rg, _, _ in conv.values() for im in rg],
                                         [m for _, mk, _ in conv.values() for m in mk], args.colors)
    pimg = palette_image(pal)
    log(f"Palette: {distinct} distinct RGB565 colours -> {len(pal)}" + (" (exact)" if exact else " (median cut)"))

    indexed = {}
    err_sum = err_n = 0
    full = [(0, 0, 0)] + pal
    for s, (rg, mk, du) in conv.items():
        idx = [index_frame(im, m, pimg, len(pal), args.dither) for im, m in zip(rg, mk)]
        indexed[s] = (idx, du)
        step = max(1, len(rg) // 8)
        for im, m, ix in zip(rg[::step], mk[::step], idx[::step]):
            d, mm = im.tobytes(), m.tobytes()
            for i in range(CELLS):
                if not mm[i]:
                    c = full[ix[i]]
                    err_sum += abs(d[3 * i] - c[0]) + abs(d[3 * i + 1] - c[1]) + abs(d[3 * i + 2] - c[2])
                    err_n += 3
    log(f"Palette mapping error: {err_sum / max(1, err_n):.2f} mean abs per channel (0-255) on avatar cells")

    # ---- frame rate, encoding, budget
    budget = int(args.budget_kb * 1024)

    def enc_one(f, data):
        off = len(data)
        blob = f
        if args.encoding != "raw":
            pb = packbits(f)
            assert unpackbits(pb) == f
            if len(pb) != CELLS and (len(pb) < CELLS or args.encoding == "rle"):
                blob = pb
        data += blob
        return off, len(blob)

    base_ms = {}
    for st, (idx, du) in indexed.items():
        fps = per.get(st, {}).get("fps", args.fps)
        base_ms[st] = 1000.0 / fps if fps > 0 else min(du)

    def encode(scale):
        """Each clip at most one frame per base_ms[state] * scale (and its frame cap)."""
        data, store, frames, clips, report = bytearray(), {}, [], {}, []
        for s in STATES:
            if s not in indexed:
                continue
            idx, du = indexed[s]
            cap = per.get(s, {}).get("max_frames", args.max_frames) or len(idx)
            cap = min(cap, max(2, int(sum(du) / (base_ms[s] * scale) + 0.5)))
            fr, dd = decimate(idx, du, cap)
            first, before = len(frames), len(data)
            for f, d in zip(fr, dd):
                while d > 0:
                    if f not in store:
                        store[f] = enc_one(f, data)
                    frames.append((*store[f], min(d, 65535)))
                    d -= 65535
            clips[s] = (first, len(frames) - first, sum(dd), s in once, slots[s][0])
            report.append((s, len(idx), len(frames) - first, sum(dd), len(data) - before))
        return bytes(data), frames, clips, report, len(store)

    scale = 1.0
    data, frames, clips, report, nuniq = encode(scale)
    fitted = False
    if len(data) > budget and not args.no_fit:
        # Every clip's frame rate down by the same factor until the data fits.
        while len(data) > budget:
            if all(r[2] <= 2 for r in report):
                die(f"can't fit {budget} bytes even at 2 frames per clip; raise --budget-kb or crop smaller")
            scale *= 1.04
            data, frames, clips, report, nuniq = encode(scale)
        fitted = True

    # ---- renderer parameters
    speak_style = {"auto": 0, "clip": 0, "bounce": 1, "frames": 2}[args.speak]
    accents = dict(DEFAULT_ACCENTS)
    working_accent = DEFAULT_WORKING_ACCENT if "working" in indexed else 0
    for a in args.accent:
        k_, _, v = a.partition("=")
        c = parse_color(v)
        v = (c[0] << 16) | (c[1] << 8) | c[2]
        if k_.lower() == "working":
            working_accent = v
        elif k_.upper() in accents:
            accents[k_.upper()] = v
        else:
            die(f"--accent: unknown mode {k_}")
    idle_crop = per.get("idle", {}).get("crop", args.crop)
    face_y = args.face_y if args.face_y is not None else (0.45 if idle_crop == "top-square" else 0.35)
    out_w, out_h, px0, py0 = place["idle"]
    av_r = max(8.0, min(28.0, 0.45 * out_w))
    full_pal = [(0, 0, 0)] + pal + FX_COLORS
    skip = {args.src, args.out, "-o", "--out"}
    options = " ".join(a for a in sys.argv[1:] if a not in skip).replace("*/", "* /") or "(defaults)"
    fields = {
        "__NAME__": args.name.replace("*/", "* /"),
        "__SOURCES__": "\n".join(f" *   {s}: {os.path.basename(slots[s][1])}" for s in STATES if s in slots),
        "__OPTIONS__": options,
        "__SPEAK__": {0: "played as is", 1: "bouncing with the voice",
                      2: "its frames picked by voice level"}[speak_style],
        "__SPEAKSTYLE__": str(speak_style),
        "__SPEAKBOUNCE__": c_float(args.speak_bounce),
        "__SPEAKPULSE__": c_float(min(0.5, max(0.0, args.speak_pulse))),
        "__HOP__": c_float(args.hop),
        "__EFFECTS__": "0" if args.no_effects else "1",
        "__HAPPYHOLD__": "1" if args.happy_hold == "clip" else "0",
        "__HAPPYDESC__": "once through from the pet" if args.happy_hold == "clip" else "while petted",
        "__AVCX__": c_float(px0 + out_w / 2 - 0.5),
        "__AVCY__": c_float(py0 + out_h * face_y),
        "__AVR__": c_float(av_r),
        "__ACCENTS__": "\n".join(f"    [MUSE_MODE_{m}] = 0x{accents[m]:06x}," for m in MODES),
        "__WORKINGACCENT__": f"0x{working_accent:06x}",
    }
    total = emit_c(args.out, fields, full_pal, data, frames, clips, args.external)

    # ---- report
    log("")
    log(f"{'state':10s} {'clip':18s} {'src fr':>6s} {'kept':>5s} {'ms':>6s} {'fps':>5s} {'bytes':>8s}")
    for s, n_src, n_kept, ms, nbytes in report:
        log(f"{s:10s} {slots[s][0]:18s} {n_src:6d} {n_kept:5d} {ms:6d} {1000 * n_kept / max(1, ms):5.1f} {nbytes:8d}"
            + ("  once" if s in once else ""))
    log("")
    log(f"Frames: {len(frames)} played, {nuniq} unique stored"
        + (f"; frame rates lowered x{1 / scale:.2f} to fit the budget" if fitted else ""))
    log(f"Frame data: {len(data)} bytes ({len(data) / 1024:.1f} KiB; {nuniq * CELLS} as raw 8bpp)")
    log(f"Const total: {total} bytes ({total / 1024:.1f} KiB) = frame data + palette {len(full_pal) * 3} "
        f"+ frame table {len(frames) * 8} + clip table {len(STATES) * 12}")
    log(f"Static RAM (bss): {CELLS} framebuffer + 512 map + 1024 palette + ~150 state")
    if len(data) > budget:
        log(f"WARNING: frame data is over the {budget}-byte budget ({args.budget_kb:g} KiB)")
    else:
        log(f"Budget: {len(data)} / {budget} bytes ({100 * len(data) / budget:.0f}%)")
    log(f"Wrote {args.out}" + (f" and {args.external} ({len(data) + 16} bytes, for the avatar partition)"
                                if args.external else ""))

    if args.dump:
        os.makedirs(args.dump, exist_ok=True)
        flat = [v for c in full_pal for v in c]
        flat += [0] * (768 - len(flat))
        for s, (first, count, _, _, _) in clips.items():
            ims, ds = [], []
            for o, ln, ms in frames[first:first + count]:
                blob = data[o:o + ln]
                im = Image.frombytes("P", (W, H), blob if ln == CELLS else unpackbits(blob))
                im.putpalette(flat)
                ims.append(im.resize((W * 5, H * 5), Image.Resampling.NEAREST))
                ds.append(ms)
            ims[0].save(os.path.join(args.dump, s + ".gif"), save_all=True, append_images=ims[1:], duration=ds, loop=0)
        log(f"Dumped the converted clips to {args.dump}")


def per_clip_options(args, slots):
    """{state: {crop, zoom, anchor, fps, max_frames, range}} from --clip-config, the --*-for flags,
    --range and --auto-crop-per-clip. Config keys may be states or clip names."""
    import json
    by_clip = {clip: st for st, (clip, _) in slots.items()}
    per = collections.defaultdict(dict)

    def state_of(k):
        k = k.strip().lower()
        if k in STATES:
            return k
        if k in by_clip:
            return by_clip[k]
        return None
    if args.clip_config:
        with open(args.clip_config) as f:
            cfg = json.load(f)
        for k, o in cfg.items():
            if k.startswith("_"):
                continue   # comments
            st = state_of(k)
            if st is None:
                log(f"  clip config: {k!r} isn't a state or a clip in use; skipped")
                continue
            for key, v in o.items():
                if key not in ("crop", "zoom", "anchor", "fps", "max_frames", "range"):
                    die(f"clip config {k}: unknown key {key!r}")
                per[st][key] = v

    def flag(items, key, conv):
        for it in items:
            k, sep, v = it.partition("=")
            st = state_of(k)
            if not sep or st is None:
                die(f"bad {key} option {it!r}: want STATE=VALUE")
            per[st][key] = conv(v)
    flag(args.crop_for, "crop", str)
    flag(args.zoom_for, "zoom", float)
    flag(args.fps_for, "fps", float)
    flag(args.range, "range", str)
    if args.auto_crop_per_clip:
        for st in slots:
            per[st].setdefault("crop", "square")
    for st, o in per.items():
        if "range" in o and isinstance(o["range"], str):
            a, _, b = o["range"].partition(":")
            o["range"] = (int(a or 0), int(b) if b else None)
        if o.get("anchor") not in (None, "top", "center", "bottom"):
            die(f"{st}: anchor must be top, center or bottom")
    return per


def trimmed(density, to_src, trim):
    """The bounding box in source pixels of the rows/columns holding at least `trim` of the
    busiest row's/column's avatar pixels, or None."""
    cols, rows = density
    if not cols or not max(cols) or not max(rows):
        return None
    fx, fy = to_src

    def span(v):
        lim = trim * max(v)
        idx = [i for i, n in enumerate(v) if n >= lim and n > 0]
        return idx[0], idx[-1] + 1
    cx0, cx1 = span(cols)
    ry0, ry1 = span(rows)
    return (cx0 * fx, ry0 * fy, cx1 * fx, ry1 * fy)


def choose_box(spec, zoom, anchor, bbox, size, to_work, trim_box, pad):
    """The crop box in working coordinates (source pixels, or art pixels for pixel art).

    bbox: the avatar's bounding box; trim_box: the same without thin sparse edges."""
    ww, wh = size
    x0, y0, x1, y1 = bbox
    if spec == "auto":
        box = bbox
    elif spec == "full":
        box = (0, 0, ww, wh)
    elif spec == "square":
        tx0, ty0, tx1, ty1 = trim_box
        side = max(tx1 - tx0, ty1 - ty0) * (1 + 2 * pad)
        cx, cy = (tx0 + tx1) / 2, (ty0 + ty1) / 2
        box = (cx - side / 2, cy - side / 2, cx + side / 2, cy + side / 2)
    elif spec == "top-square":
        side = x1 - x0
        box = (x0, y0, x1, y0 + side)
        anchor = anchor or "top"
    else:
        try:
            x, y, w, h = (float(v) for v in spec.split(","))
        except ValueError:
            die(f"crop {spec!r}: want auto, square, top-square, full or X,Y,W,H")
        bx0, by0 = to_work(x, y)
        bx1, by1 = to_work(x + w, y + h)
        box = (bx0, by0, bx1, by1)
    anchor = anchor or "center"
    if zoom != 1.0:
        bx0, by0, bx1, by1 = box
        w, h = (bx1 - bx0) / zoom, (by1 - by0) / zoom
        cx = (bx0 + bx1) / 2
        y = by0 if anchor == "top" else (by1 - h if anchor == "bottom" else (by0 + by1) / 2 - h / 2)
        box = (cx - w / 2, y, cx + w / 2, y + h)
    if box[2] - box[0] < 1 or box[3] - box[1] < 1:
        die("crop box is empty")
    return box


if __name__ == "__main__":
    main()
